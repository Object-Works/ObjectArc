#pragma once

#include <optional>
#include <string>

namespace oa {

// Folds full-width ASCII (as typed with a Japanese IME) to half-width and lowercases.
std::wstring Normalize(const std::wstring& s);

// How well `query` matches `text`, both normalized: 0 = no match, up to 1000 = identical.
// Prefix > word start > substring > scattered letters ("vsc" for "Visual Studio Code").
// Space-separated words in `query` must each match.
int MatchScore(const std::wstring& query, const std::wstring& text);

// Evaluates arithmetic such as "2^10 / 3" or "sqrt(2) * pi". Plain numbers aren't expressions.
std::optional<double> Calculate(const std::wstring& normalizedQuery);
std::wstring FormatNumber(double value);

}  // namespace oa
