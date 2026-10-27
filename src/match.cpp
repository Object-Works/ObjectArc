#include "match.h"

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace oa {

std::wstring Normalize(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = (wchar_t)(c - 0xFEE0);
        else if (c == 0x3000) c = L' ';
        else if (c == L'×') c = L'*';
        else if (c == L'÷') c = L'/';
        c = (wchar_t)std::towlower(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

namespace {

bool IsBoundary(const std::wstring& t, size_t i) {
    if (i == 0) return true;
    const wchar_t p = t[i - 1], c = t[i];
    if (p == L' ' || p == L'-' || p == L'_' || p == L'.' || p == L'(' || p == L'/' || p == L'\\') return true;
    return std::iswdigit(c) && !std::iswdigit(p);
}

int Scattered(const std::wstring& q, const std::wstring& t, bool preferBoundaries) {
    int bonus = 0, prev = -2;
    size_t from = 0;
    for (wchar_t qc : q) {
        if (qc == L' ') continue;
        size_t found = t.find(qc, from);
        if (found == std::wstring::npos) return 0;
        if (preferBoundaries && (int)found != prev + 1 && !IsBoundary(t, found)) {
            for (size_t j = found + 1; j < t.size(); ++j) {
                if (t[j] == qc && IsBoundary(t, j)) {
                    found = j;
                    break;
                }
            }
        }
        if (IsBoundary(t, found)) bonus += 30;
        if ((int)found == prev + 1) bonus += 20;
        else if (prev >= 0) bonus -= std::min((int)found - prev - 1, 10);
        prev = (int)found;
        from = found + 1;
    }
    return std::clamp(300 + bonus, 1, 600);
}

int MatchWord(const std::wstring& q, const std::wstring& t) {
    if (q.empty()) return 1;
    if (t == q) return 1000;
    if (t.rfind(q, 0) == 0) return 900 - (int)std::min<size_t>(100, t.size() - q.size());
    size_t first = t.find(q);
    for (size_t pos = first; pos != std::wstring::npos; pos = t.find(q, pos + 1)) {
        if (IsBoundary(t, pos)) return 800 - (int)std::min<size_t>(100, pos);
    }
    if (first != std::wstring::npos) return 650 - (int)std::min<size_t>(100, first);
    if (q.size() < 2) return 0;  // a single scattered letter matches nearly everything
    int s = Scattered(q, t, true);
    return s ? s : Scattered(q, t, false);
}

}  // namespace

int MatchScore(const std::wstring& query, const std::wstring& text) {
    const int whole = MatchWord(query, text);
    if (whole >= 650 || query.find(L' ') == std::wstring::npos) return whole;
    int total = 0, words = 0;
    for (auto& w : Split(query, L' ')) {
        if (w.empty()) continue;
        const int s = MatchWord(w, text);
        if (!s) return whole;
        total += s;
        ++words;
    }
    return words ? std::max(whole, total / words * 9 / 10) : whole;
}

// ---------------------------------------------------------------------------
// Calculator
// ---------------------------------------------------------------------------

namespace {

constexpr double kPi = 3.14159265358979323846;

class Parser {
public:
    explicit Parser(const std::wstring& s) : s_(s) {}

    std::optional<double> Run() {
        double v = Sum();
        Skip();
        if (failed_ || pos_ != s_.size() || !operators_) return std::nullopt;
        if (!std::isfinite(v)) return std::nullopt;
        return v;
    }

private:
    void Skip() {
        while (pos_ < s_.size() && (s_[pos_] == L' ' || s_[pos_] == L',')) ++pos_;
    }
    bool Eat(wchar_t c) {
        Skip();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    double Sum() {
        double v = Product();
        for (;;) {
            if (Eat(L'+')) v += Product();
            else if (Eat(L'-')) v -= Product();
            else return v;
            operators_ = true;
        }
    }

    double Product() {
        double v = Power();
        for (;;) {
            if (Eat(L'*') || Eat(L'x')) v *= Power();
            else if (Eat(L'/')) v /= Power();
            else if (Eat(L'%')) v = std::fmod(v, Power());
            else return v;
            operators_ = true;
        }
    }

    double Power() {
        double base = Unary();
        if (Eat(L'^')) {
            operators_ = true;
            return std::pow(base, Power());  // right-associative
        }
        return base;
    }

    double Unary() {
        if (Eat(L'-')) return -Unary();
        if (Eat(L'+')) return Unary();
        return Atom();
    }

    double Atom() {
        Skip();
        if (Eat(L'(')) {
            operators_ = true;
            double v = Sum();
            if (!Eat(L')')) failed_ = true;
            return v;
        }
        if (pos_ < s_.size() && (std::iswdigit(s_[pos_]) || s_[pos_] == L'.')) {
            const wchar_t* start = s_.c_str() + pos_;
            wchar_t* end = nullptr;
            double v = std::wcstod(start, &end);
            if (end == start) failed_ = true;
            pos_ += end - start;
            return v;
        }
        size_t b = pos_;
        while (pos_ < s_.size() && std::iswalpha(s_[pos_])) ++pos_;
        const std::wstring name = s_.substr(b, pos_ - b);
        if (name == L"pi") {
            operators_ = true;
            return kPi;
        }
        if (name == L"e") {
            operators_ = true;
            return std::exp(1.0);
        }
        static const struct {
            const wchar_t* name;
            double (*fn)(double);
        } kFunctions[] = {
            {L"sqrt", [](double x) { return std::sqrt(x); }},  {L"abs", [](double x) { return std::fabs(x); }},
            {L"sin", [](double x) { return std::sin(x); }},    {L"cos", [](double x) { return std::cos(x); }},
            {L"tan", [](double x) { return std::tan(x); }},    {L"asin", [](double x) { return std::asin(x); }},
            {L"acos", [](double x) { return std::acos(x); }},  {L"atan", [](double x) { return std::atan(x); }},
            {L"log", [](double x) { return std::log10(x); }},  {L"ln", [](double x) { return std::log(x); }},
            {L"exp", [](double x) { return std::exp(x); }},    {L"round", [](double x) { return std::round(x); }},
            {L"floor", [](double x) { return std::floor(x); }}, {L"ceil", [](double x) { return std::ceil(x); }},
        };
        for (const auto& f : kFunctions) {
            if (name == f.name) {
                operators_ = true;
                if (!Eat(L'(')) {
                    failed_ = true;
                    return 0;
                }
                double v = Sum();
                if (!Eat(L')')) failed_ = true;
                return f.fn(v);
            }
        }
        failed_ = true;
        return 0;
    }

    const std::wstring& s_;
    size_t pos_ = 0;
    bool failed_ = false;
    bool operators_ = false;
};

}  // namespace

std::optional<double> Calculate(const std::wstring& query) {
    std::wstring q = Trim(query);
    if (!q.empty() && q[0] == L'=') q = q.substr(1);
    if (q.empty() || q.find_first_of(L"0123456789") == std::wstring::npos) {
        if (q != L"pi" && q != L"e") return std::nullopt;
    }
    return Parser(q).Run();
}

std::wstring FormatNumber(double v) {
    if (v == 0) v = 0;  // no "-0"
    wchar_t buf[64];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) swprintf_s(buf, L"%.0f", v);
    else swprintf_s(buf, L"%.12g", v);
    return buf;
}

}  // namespace oa
