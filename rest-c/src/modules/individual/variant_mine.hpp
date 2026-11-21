#pragma once
// ============================================================================
// ВАРИАНТ 128: Модуль планирования фонда оплаты труда (ФОТ).
//
// Вход: пять чисел в порядке задания — массивом
//     [численность, средняя_зарплата, коэффициент_премий,
//      количество_месяцев, налоговая_нагрузка]
// или объектом с теми же ключами:
//     {"численность": 25, "средняя_зарплата": 40000, "коэффициент_премий": 1.15,
//      "количество_месяцев": 12, "налоговая_нагрузка": 0.3}
// Выход: массив из пяти строк в формате задания:
//     ["Годовой ФОТ: X", "Средняя зарплата: Y", "Налоги: Z",
//      "Чистый ФОТ: W", "Налоговая нагрузка: V%"]
//
// Алгоритм (по заданию):
//     ФОТ_база   = численность × средняя_зарплата × количество_месяцев
//     ФОТ_общий  = ФОТ_база × коэффициент_премий
//     Налоги     = ФОТ_общий × налоговая_нагрузка
//     ФОТ_чистый = ФОТ_общий − Налоги
//     Средняя    = ФОТ_общий / (численность × количество_месяцев)
//     V          = налоговая_нагрузка × 100
//
// Все проверки выполняются ДО расчёта. Суммы считаются точно в десятичной
// записи и округляются до копеек по правилу «половина вверх».
//
// Имя namespace TaskVariant менять нельзя: на него опирается task.hpp.
// Зависимости: только стандартная библиотека C++ и json.hpp проекта.
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>
#include "../../json.hpp"

namespace TaskVariant {

    // ------------------------------------------------------------------------
    // Константы задачи.
    // ------------------------------------------------------------------------
    inline constexpr int    FIELD_COUNT       = 5;   // число входных параметров
    inline constexpr size_t MAX_NUMBER_LENGTH = 64;  // предельная длина записи числа

    // Порядковые номера параметров (порядок массива в примерах задания).
    enum FieldId { HEADCOUNT = 0, SALARY = 1, BONUS = 2, MONTHS = 3, TAX_LOAD = 4 };

    // Описание параметра: имя (ключ JSON), границы в десятичной записи,
    // требование целого значения. Границы сравниваются с точной записью числа.
    struct FieldSpec {
        const char* name;
        const char* min;
        const char* max;
        bool        integer;
        const char* range_text;
    };

    inline constexpr FieldSpec SPECS[FIELD_COUNT] = {
        {"численность",        "1",    "1000000",    true,  "от 1 до 1000000"},
        {"средняя_зарплата",   "0.01", "1000000000", false, "от 0.01 до 1000000000"},
        {"коэффициент_премий", "1",    "5",          false, "от 1 до 5"},
        {"количество_месяцев", "1",    "12",         true,  "от 1 до 12"},
        {"налоговая_нагрузка", "0",    "1",          false, "от 0 до 1"},
    };

    inline const std::string MSG_FORMAT =
        "Тело запроса должно быть JSON-массивом из пяти чисел "
        "или JSON-объектом с полями варианта";
    inline const std::string MSG_ARRAY_SIZE =
        "Массив должен содержать ровно пять чисел: [численность, средняя_зарплата, "
        "коэффициент_премий, количество_месяцев, налоговая_нагрузка]";
    inline const std::string MSG_TOO_LONG =
        "Слишком длинная запись числа (более 64 символов)";

    // ------------------------------------------------------------------------
    // Одно поле запроса. Отдельные признаки нужны, чтобы отличить
    // «поле не передано» от «передан 0» и число от строки.
    // ------------------------------------------------------------------------
    struct Field {
        bool        present   = false;  // ключ (или элемент массива) найден
        bool        is_number = false;  // значение является числом JSON
        std::string text;               // запись числа для проверок и расчёта
    };

    // ------------------------------------------------------------------------
    // Структура входных данных.
    // ------------------------------------------------------------------------
    struct Input {
        Field       fields[FIELD_COUNT];  // параметры в порядке FieldId
        std::string format_error;         // ошибка структуры тела запроса
    };

    // ------------------------------------------------------------------------
    // Структура выходных данных: суммы в рублях без лишних нулей.
    // ------------------------------------------------------------------------
    struct Output {
        std::string fot_total;   // годовой ФОТ (ФОТ_общий)
        std::string avg_salary;  // средняя зарплата с премией
        std::string taxes;       // налоги и взносы
        std::string net_fot;     // чистый ФОТ
        std::string tax_load;    // налоговая нагрузка, %
    };

    namespace detail {

        // ====================================================================
        // Разбор JSON: плоский объект или массив значений.
        // ====================================================================
        inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

        inline void skip_spaces(const std::string& s, size_t& pos) {
            while (pos < s.size() &&
                   (s[pos] == ' ' || s[pos] == '\t' ||
                    s[pos] == '\n' || s[pos] == '\r'))
                ++pos;
        }

        // Число по грамматике JSON: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
        // Отсекает то, что принял бы strtod: inf, nan, 0x10, +5, .5, 5., 01.
        inline bool read_number(const std::string& s, size_t& pos, std::string& text) {
            size_t start = pos;
            if (pos < s.size() && s[pos] == '-') ++pos;
            if (pos >= s.size() || !is_digit(s[pos])) { pos = start; return false; }
            if (s[pos] == '0') {
                ++pos;
            } else {
                while (pos < s.size() && is_digit(s[pos])) ++pos;
            }
            if (pos < s.size() && s[pos] == '.') {
                ++pos;
                if (pos >= s.size() || !is_digit(s[pos])) { pos = start; return false; }
                while (pos < s.size() && is_digit(s[pos])) ++pos;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                if (pos >= s.size() || !is_digit(s[pos])) { pos = start; return false; }
                while (pos < s.size() && is_digit(s[pos])) ++pos;
            }
            text = s.substr(start, pos - start);
            return true;
        }

        // Кодовая точка Unicode в UTF-8 (для экранирования \uXXXX).
        inline void append_utf8(std::string& out, unsigned long cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        inline bool read_hex4(const std::string& s, size_t& pos, unsigned long& cp) {
            if (pos + 4 > s.size()) return false;
            cp = 0;
            for (int i = 0; i < 4; ++i) {
                char c = s[pos++];
                cp <<= 4;
                int h;
                if (c >= '0' && c <= '9')      h = c - '0';
                else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') h = c - 'A' + 10;
                else return false;
                cp |= static_cast<unsigned long>(h);
            }
            return true;
        }

        // Строка JSON в двойных кавычках с разбором экранирования.
        inline bool read_string(const std::string& s, size_t& pos, std::string& out) {
            if (pos >= s.size() || s[pos] != '"') return false;
            ++pos;
            out.clear();
            while (pos < s.size() && s[pos] != '"') {
                char c = s[pos++];
                if (c != '\\') { out += c; continue; }
                if (pos >= s.size()) return false;
                char e = s[pos++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned long cp = 0;
                        if (!read_hex4(s, pos, cp)) return false;
                        // Суррогатная пара UTF-16 -> одна кодовая точка.
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() &&
                            s[pos] == '\\' && s[pos + 1] == 'u') {
                            size_t save = pos;
                            pos += 2;
                            unsigned long low = 0;
                            if (read_hex4(s, pos, low) &&
                                low >= 0xDC00 && low <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            else
                                pos = save;
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            }
            if (pos >= s.size()) return false;
            ++pos;  // закрывающая кавычка
            return true;
        }

        // Значение поля: число, строка, true/false/null. Вложенные объекты и
        // массивы по заданию не нужны и считаются ошибкой формата.
        inline bool read_value(const std::string& s, size_t& pos, Field& field,
                               std::string& error) {
            field.present = true;
            std::string text;
            if (read_number(s, pos, text)) {
                if (text.size() > MAX_NUMBER_LENGTH) {
                    error = MSG_TOO_LONG;
                    return false;
                }
                field.is_number = true;
                field.text      = text;
                return true;
            }
            if (read_string(s, pos, text)) return true;
            for (const char* word : {"true", "false", "null"}) {
                std::string w(word);
                if (s.compare(pos, w.size(), w) == 0) { pos += w.size(); return true; }
            }
            error = MSG_FORMAT;
            return false;
        }

        // Номер параметра по ключу JSON или -1, если ключ не из задания.
        inline int field_index(const std::string& key) {
            for (int i = 0; i < FIELD_COUNT; ++i)
                if (key == SPECS[i].name) return i;
            return -1;
        }

        inline bool parse_object(const std::string& s, size_t& pos, Input& in) {
            ++pos;  // '{'
            skip_spaces(s, pos);
            if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
            while (true) {
                skip_spaces(s, pos);
                std::string key;
                if (!read_string(s, pos, key)) {
                    in.format_error = MSG_FORMAT;
                    return false;
                }
                skip_spaces(s, pos);
                if (pos >= s.size() || s[pos] != ':') {
                    in.format_error = MSG_FORMAT;
                    return false;
                }
                ++pos;
                skip_spaces(s, pos);
                int idx = field_index(key);
                if (idx < 0) {
                    in.format_error = "Неизвестное поле: " + key;
                    return false;
                }
                if (in.fields[idx].present) {
                    in.format_error = "Поле указано повторно: " + key;
                    return false;
                }
                if (!read_value(s, pos, in.fields[idx], in.format_error)) return false;
                skip_spaces(s, pos);
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
                in.format_error = MSG_FORMAT;
                return false;
            }
        }

        inline bool parse_array(const std::string& s, size_t& pos, Input& in) {
            ++pos;  // '['
            skip_spaces(s, pos);
            int count = 0;
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
            } else {
                while (true) {
                    skip_spaces(s, pos);
                    Field extra;  // элементы сверх пятого разбираются и отбрасываются
                    Field& target = count < FIELD_COUNT ? in.fields[count] : extra;
                    if (!read_value(s, pos, target, in.format_error)) return false;
                    ++count;
                    skip_spaces(s, pos);
                    if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                    if (pos < s.size() && s[pos] == ']') { ++pos; break; }
                    in.format_error = MSG_FORMAT;
                    return false;
                }
            }
            if (count != FIELD_COUNT) {
                in.format_error = MSG_ARRAY_SIZE;
                return false;
            }
            return true;
        }

        // ====================================================================
        // Точная десятичная арифметика: значение = digits × 10^(−scale).
        // Тип double для сумм не подходит: при умножении копейки, равные
        // ровно половине, теряются из-за двоичного представления чисел.
        // ====================================================================
        struct Dec {
            std::string digits = "0";  // десятичные цифры без ведущих нулей
            long        scale  = 0;    // число цифр дробной части
        };

        inline void normalize(Dec& d) {
            size_t nz = d.digits.find_first_not_of('0');
            d.digits = (nz == std::string::npos) ? "0" : d.digits.substr(nz);
            if (d.digits == "0") { d.scale = 0; return; }
            if (d.scale < 0) {
                d.digits.append(static_cast<size_t>(-d.scale), '0');
                d.scale = 0;
            }
        }

        // Запись числа JSON (уже проверенная) -> Dec. Знак не учитывается:
        // после проверки диапазонов значения неотрицательны (−0 даёт 0).
        inline Dec to_dec(const std::string& text) {
            Dec d;
            d.digits.clear();
            size_t i = 0;
            if (i < text.size() && text[i] == '-') ++i;
            long frac_len = 0;
            bool in_frac = false;
            for (; i < text.size() && text[i] != 'e' && text[i] != 'E'; ++i) {
                if (text[i] == '.') { in_frac = true; continue; }
                d.digits += text[i];
                if (in_frac) ++frac_len;
            }
            long exp = 0;
            if (i < text.size()) {
                ++i;  // 'e'
                bool neg = false;
                if (i < text.size() && (text[i] == '+' || text[i] == '-'))
                    neg = (text[i++] == '-');
                for (; i < text.size(); ++i)
                    if (exp < 100000) exp = exp * 10 + (text[i] - '0');  // насыщение
                if (neg) exp = -exp;
            }
            if (d.digits.empty()) d.digits = "0";
            d.scale = frac_len - exp;
            normalize(d);
            return d;
        }

        // Отрицательна ли запись числа (−0 отрицательной не считается).
        inline bool is_negative(const std::string& text) {
            return !text.empty() && text[0] == '-' && to_dec(text).digits != "0";
        }

        // Есть ли у числа ненулевая дробная часть.
        inline bool is_integer(const Dec& d) {
            if (d.scale <= 0) return true;
            size_t frac = static_cast<size_t>(d.scale);
            if (d.digits.size() <= frac) return d.digits == "0";
            return d.digits.find_first_not_of('0', d.digits.size() - frac) ==
                   std::string::npos;
        }

        // Цифры с дописанными справа нулями, без ведущих нулей.
        inline std::string pad_right(const Dec& d, long scale) {
            size_t zeros = static_cast<size_t>(scale - d.scale);
            std::string r = d.digits + std::string(zeros, '0');
            size_t nz = r.find_first_not_of('0');
            return nz == std::string::npos ? "" : r.substr(nz);
        }

        // Сравнение неотрицательных чисел: −1 (a < b), 0 (a = b) или 1 (a > b).
        inline int cmp(const Dec& a, const Dec& b) {
            long scale = a.scale > b.scale ? a.scale : b.scale;
            std::string x = pad_right(a, scale);
            std::string y = pad_right(b, scale);
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            int c = x.compare(y);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }

        inline Dec mul(const Dec& a, const Dec& b) {
            std::vector<int> r(a.digits.size() + b.digits.size(), 0);
            for (size_t i = a.digits.size(); i-- > 0;) {
                for (size_t j = b.digits.size(); j-- > 0;) {
                    int p = (a.digits[i] - '0') * (b.digits[j] - '0') + r[i + j + 1];
                    r[i + j + 1] = p % 10;
                    r[i + j]    += p / 10;
                }
            }
            Dec d;
            d.digits.clear();
            for (int x : r) d.digits += static_cast<char>('0' + x);
            d.scale = a.scale + b.scale;
            normalize(d);
            return d;
        }

        // Прибавить 1 к неотрицательному целому в десятичной записи.
        inline std::string inc(std::string s) {
            for (size_t i = s.size(); i-- > 0;) {
                if (s[i] < '9') { ++s[i]; return s; }
                s[i] = '0';
            }
            return "1" + s;
        }

        // Округление до сотых по правилу «половина вверх».
        // Результат — целое число сотых (копеек) в десятичной записи.
        inline std::string round_cents(const Dec& d) {
            if (d.scale <= 2)
                return d.digits == "0" ? "0" : d.digits + std::string(2 - d.scale, '0');
            size_t cut = static_cast<size_t>(d.scale - 2);  // сколько цифр отбросить
            std::string head;
            char next = '0';
            if (d.digits.size() > cut) {
                head = d.digits.substr(0, d.digits.size() - cut);
                next = d.digits[d.digits.size() - cut];
            } else {
                head = "0";
                if (d.digits.size() == cut) next = d.digits[0];
            }
            if (next >= '5') head = inc(head);
            return head;
        }

        // Разность неотрицательных целых a − b при a >= b.
        inline std::string sub(const std::string& a, const std::string& b) {
            std::string r = a;
            int borrow = 0;
            for (size_t i = 0; i < r.size(); ++i) {
                size_t ia = r.size() - 1 - i;
                int y = i < b.size() ? b[b.size() - 1 - i] - '0' : 0;
                int x = (r[ia] - '0') - borrow - y;
                borrow = x < 0 ? 1 : 0;
                if (x < 0) x += 10;
                r[ia] = static_cast<char>('0' + x);
            }
            size_t nz = r.find_first_not_of('0');
            return nz == std::string::npos ? "0" : r.substr(nz);
        }

        // Копейки -> рубли без лишних нулей: "720000000" -> "7200000",
        // "1" -> "0.01", "3020" -> "30.2".
        inline std::string cents_to_text(const std::string& c) {
            std::string s = c.size() < 3 ? std::string(3 - c.size(), '0') + c : c;
            std::string whole = s.substr(0, s.size() - 2);
            std::string frac  = s.substr(s.size() - 2);
            while (!frac.empty() && frac.back() == '0') frac.pop_back();
            return frac.empty() ? whole : whole + "." + frac;
        }

    }  // namespace detail

    // ------------------------------------------------------------------------
    // Разбор JSON-тела запроса в Input.
    // ------------------------------------------------------------------------
    inline Input parse(const std::string& body) {
        Input in;
        size_t pos = 0;
        detail::skip_spaces(body, pos);
        bool ok = false;
        if (pos < body.size() && body[pos] == '{')
            ok = detail::parse_object(body, pos, in);
        else if (pos < body.size() && body[pos] == '[')
            ok = detail::parse_array(body, pos, in);
        if (ok) {
            detail::skip_spaces(body, pos);
            if (pos != body.size()) ok = false;  // лишние символы после JSON
        }
        if (!ok) {
            std::string err = in.format_error.empty() ? MSG_FORMAT : in.format_error;
            in = Input{};
            in.format_error = err;
        }
        return in;
    }

    // ------------------------------------------------------------------------
    // Проверка входных данных. Возвращает "" если всё ок, иначе — текст ошибки.
    // Порядок: структура тела -> наличие полей -> типы -> целые -> диапазоны.
    // ------------------------------------------------------------------------
    inline std::string validate(const Input& in) {
        if (!in.format_error.empty())
            return in.format_error;
        for (int i = 0; i < FIELD_COUNT; ++i)
            if (!in.fields[i].present)
                return std::string("Не задан параметр ") + SPECS[i].name;
        for (int i = 0; i < FIELD_COUNT; ++i)
            if (!in.fields[i].is_number)
                return std::string("Параметр ") + SPECS[i].name + " должен быть числом";
        // Целочисленность и диапазон проверяются по точной записи числа (Dec),
        // поэтому 10.0000000000000001 не целое, а 5.0000000000000000001 больше 5.
        for (int i = 0; i < FIELD_COUNT; ++i)
            if (SPECS[i].integer &&
                !detail::is_integer(detail::to_dec(in.fields[i].text)))
                return std::string("Параметр ") + SPECS[i].name +
                       " должен быть целым числом";
        for (int i = 0; i < FIELD_COUNT; ++i) {
            const std::string& t = in.fields[i].text;
            detail::Dec v = detail::to_dec(t);
            bool below = detail::is_negative(t) ||
                         detail::cmp(v, detail::to_dec(SPECS[i].min)) < 0;
            bool above = detail::cmp(v, detail::to_dec(SPECS[i].max)) > 0;
            if (below || above)
                return std::string("Параметр ") + SPECS[i].name +
                       " должен быть в диапазоне " + SPECS[i].range_text;
        }
        return "";
    }

    // ------------------------------------------------------------------------
    // Основные расчёты (шаги 1–4 алгоритма задания и показатель шага 5).
    // ------------------------------------------------------------------------
    inline Output calculate(const Input& in) {
        using namespace detail;
        Dec n = to_dec(in.fields[HEADCOUNT].text);
        Dec s = to_dec(in.fields[SALARY].text);
        Dec k = to_dec(in.fields[BONUS].text);
        Dec m = to_dec(in.fields[MONTHS].text);
        Dec t = to_dec(in.fields[TAX_LOAD].text);

        Dec fot_base  = mul(mul(n, s), m);   // шаг 1
        Dec fot_total = mul(fot_base, k);    // шаг 2
        Dec taxes     = mul(fot_total, t);   // шаг 3
        // Средняя = ФОТ_общий / (N × M) = средняя_зарплата × коэффициент_премий.
        Dec avg       = mul(s, k);
        Dec percent   = t;
        percent.scale -= 2;                  // × 100
        normalize(percent);

        std::string total_c = round_cents(fot_total);
        std::string taxes_c = round_cents(taxes);

        Output out;
        out.fot_total  = cents_to_text(total_c);
        out.avg_salary = cents_to_text(round_cents(avg));
        out.taxes      = cents_to_text(taxes_c);
        out.net_fot    = cents_to_text(sub(total_c, taxes_c));  // шаг 4, в копейках
        out.tax_load   = cents_to_text(round_cents(percent));
        return out;
    }

    // ------------------------------------------------------------------------
    // Формирование JSON-ответа: массив из пяти строк в формате задания.
    // ------------------------------------------------------------------------
    inline std::string to_json(const Output& out) {
        const std::string lines[FIELD_COUNT] = {
            "Годовой ФОТ: "        + out.fot_total,
            "Средняя зарплата: "   + out.avg_salary,
            "Налоги: "             + out.taxes,
            "Чистый ФОТ: "         + out.net_fot,
            "Налоговая нагрузка: " + out.tax_load + "%",
        };
        JArr arr;
        for (const auto& line : lines) arr.add("\"" + jesc(line) + "\"");
        return arr.str();
    }

    // ========================================================================
    // Автотесты модуля (20 проверок) для /admin/run-tests-individual.
    // Каждый тест проходит ту же цепочку parse -> validate -> calculate ->
    // to_json, что и маршрут /works/calc-individual.
    // ========================================================================
    struct Result {
        bool        ok = false;   // расчёт выполнен (иначе ошибка валидации)
        std::string error;        // текст ошибки
        std::string json;         // ответ модуля
    };

    inline Result process(const std::string& body) {
        Result r;
        Input in = parse(body);
        r.error = validate(in);
        if (!r.error.empty()) return r;
        r.ok   = true;
        r.json = to_json(calculate(in));
        return r;
    }

    struct TestCase {
        std::string name;
        std::string group;
        bool        passed;
    };

    inline std::vector<TestCase> run_self_tests() {
        auto expect = [](const std::string& body, const std::string& json) {
            Result r = process(body);
            return r.ok && r.json == json;
        };
        auto reject = [](const std::string& body, const std::string& error) {
            Result r = process(body);
            return !r.ok && r.error == error;
        };
        const std::string range_n =
            "Параметр численность должен быть в диапазоне от 1 до 1000000";
        const std::string range_s =
            "Параметр средняя_зарплата должен быть в диапазоне от 0.01 до 1000000000";
        const std::string range_k =
            "Параметр коэффициент_премий должен быть в диапазоне от 1 до 5";
        const std::string range_m =
            "Параметр количество_месяцев должен быть в диапазоне от 1 до 12";
        const std::string ex2 =
            "[\"Годовой ФОТ: 13800000\",\"Средняя зарплата: 46000\","
            "\"Налоги: 4140000\",\"Чистый ФОТ: 9660000\",\"Налоговая нагрузка: 30%\"]";

        const char* OK  = "корректные данные";
        const char* BND = "граничные значения";
        const char* BAD = "неверные данные";
        const char* INC = "неполные данные";

        return {
            {"Пример 2: [25, 40000, 1.15, 12, 0.3]", OK,
             expect("[25, 40000, 1.15, 12, 0.3]", ex2)},
            {"Пример 1 по алгоритму: [10, 50000, 1.2, 12, 0.3] -> 7200000", OK,
             expect("[10, 50000, 1.2, 12, 0.3]",
                    "[\"Годовой ФОТ: 7200000\",\"Средняя зарплата: 60000\","
                    "\"Налоги: 2160000\",\"Чистый ФОТ: 5040000\","
                    "\"Налоговая нагрузка: 30%\"]")},
            {"Коэффициент 1.3 даёт выход примера 1 задания", OK,
             expect("[10, 50000, 1.3, 12, 0.3]",
                    "[\"Годовой ФОТ: 7800000\",\"Средняя зарплата: 65000\","
                    "\"Налоги: 2340000\",\"Чистый ФОТ: 5460000\","
                    "\"Налоговая нагрузка: 30%\"]")},
            {"Объект с ключами задания равен массиву (пример 2)", OK,
             expect("{\"численность\": 25, \"средняя_зарплата\": 40000, "
                    "\"коэффициент_премий\": 1.15, \"количество_месяцев\": 12, "
                    "\"налоговая_нагрузка\": 0.3}", ex2)},
            {"Копейки: чистый ФОТ = ФОТ - налоги после округления", OK,
             expect("[7, 12345.67, 1.37, 5, 0.302]",
                    "[\"Годовой ФОТ: 591974.88\",\"Средняя зарплата: 16913.57\","
                    "\"Налоги: 178776.41\",\"Чистый ФОТ: 413198.47\","
                    "\"Налоговая нагрузка: 30.2%\"]")},
            {"Половина копейки округляется вверх: 179245646.195 -> .20", OK,
             expect("[2933, 53142.10, 1.15, 1, 0.164]",
                    "[\"Годовой ФОТ: 179245646.2\",\"Средняя зарплата: 61113.42\","
                    "\"Налоги: 29396285.98\",\"Чистый ФОТ: 149849360.22\","
                    "\"Налоговая нагрузка: 16.4%\"]")},
            {"Все нижние границы: [1, 0.01, 1, 1, 0]", BND,
             expect("[1, 0.01, 1, 1, 0]",
                    "[\"Годовой ФОТ: 0.01\",\"Средняя зарплата: 0.01\",\"Налоги: 0\","
                    "\"Чистый ФОТ: 0.01\",\"Налоговая нагрузка: 0%\"]")},
            {"Все верхние границы: [1000000, 1000000000, 5, 12, 1]", BND,
             expect("[1000000, 1000000000, 5, 12, 1]",
                    "[\"Годовой ФОТ: 60000000000000000\","
                    "\"Средняя зарплата: 5000000000\","
                    "\"Налоги: 60000000000000000\",\"Чистый ФОТ: 0\","
                    "\"Налоговая нагрузка: 100%\"]")},
            {"Численность 0 отклоняется", BND,
             reject("[0, 40000, 1.15, 12, 0.3]", range_n)},
            {"Численность 10.0000000000000001 не целая", BND,
             reject("[10.0000000000000001, 40000, 1.15, 12, 0.3]",
                    "Параметр численность должен быть целым числом")},
            {"Средняя зарплата 0.0099999999999999999 отклоняется", BND,
             reject("[25, 0.0099999999999999999, 1.15, 12, 0.3]", range_s)},
            {"Коэффициент премий 5.01 отклоняется", BND,
             reject("[25, 40000, 5.01, 12, 0.3]", range_k)},
            {"Количество месяцев 13 отклоняется", BND,
             reject("[25, 40000, 1.15, 13, 0.3]", range_m)},
            {"Налоговая нагрузка 1.01 отклоняется", BND,
             reject("[25, 40000, 1.15, 12, 1.01]",
                    "Параметр налоговая_нагрузка должен быть в диапазоне от 0 до 1")},
            {"Численность строкой", BAD,
             reject("[\"25\", 40000, 1.15, 12, 0.3]",
                    "Параметр численность должен быть числом")},
            {"Дробная численность 10.5", BAD,
             reject("[10.5, 40000, 1.15, 12, 0.3]",
                    "Параметр численность должен быть целым числом")},
            {"Лишние символы после JSON", BAD,
             reject("[25, 40000, 1.15, 12, 0.3] x", MSG_FORMAT)},
            {"Пустое тело запроса", INC,
             reject("", MSG_FORMAT)},
            {"Массив из четырёх чисел", INC,
             reject("[25, 40000, 1.15, 12]", MSG_ARRAY_SIZE)},
            {"Объект без поля налоговая_нагрузка", INC,
             reject("{\"численность\": 25, \"средняя_зарплата\": 40000, "
                    "\"коэффициент_премий\": 1.15, \"количество_месяцев\": 12}",
                    "Не задан параметр налоговая_нагрузка")},
        };
    }

} // namespace TaskVariant
