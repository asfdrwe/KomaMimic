// SPDX-License-Identifier: GPL-3.0-only
#include "expression.h"
#include <QCoreApplication>
#include <QVector>
#include <algorithm>
#include <cmath>
namespace {
class Parser {
public:
  QString s, error;
  QHash<QString, double> vars;
  int at = 0, depth = 0, steps = 0;
  bool take(const QString &token) {
    while (at < s.size() && s[at].isSpace())
      ++at;
    if (s.mid(at, token.size()) == token) {
      at += token.size();
      return true;
    }
    return false;
  }
  double expr(int minimum = 0) {
    if (++depth > 64 || ++steps > 4096) {
      error = QCoreApplication::translate("KomaMimic", "式が複雑すぎます。");
      --depth;
      return 0;
    }
    double left = 0;
    if (take("+"))
      left = expr(8);
    else if (take("-"))
      left = -expr(8);
    else if (take("!"))
      left = !expr(8);
    else if (take("(")) {
      left = expr();
      if (!take(")"))
        error = QCoreApplication::translate("KomaMimic", "閉じ括弧がありません。");
    } else {
      while (at < s.size() && s[at].isSpace())
        ++at;
      if (at < s.size() && (s[at].isDigit() || s[at] == '.')) {
        int first = at;
        while (at < s.size() && (s[at].isDigit() || s[at] == '.'))
          ++at;
        bool ok = false;
        left = s.mid(first, at - first).toDouble(&ok);
        if (!ok)
          error = QCoreApplication::translate("KomaMimic", "数値が不正です。");
      } else {
        take("@");
        int first = at;
        while (at < s.size() && (s[at].isLetterOrNumber() || s[at] == '_'))
          ++at;
        const auto name = s.mid(first, at - first);
        if (name.isEmpty())
          error = QCoreApplication::translate("KomaMimic", "値が必要です。");
        else if (take("(")) {
          QVector<double> a;
          if (!take(")")) {
            do {
              a.append(expr());
            } while (error.isEmpty() && a.size() < 4 && take(","));
            if (!take(")"))
              error = QCoreApplication::translate("KomaMimic", "関数の引数が不正です。");
          }
          if (name == "min" && a.size() == 2)
            left = std::min(a[0], a[1]);
          else if (name == "max" && a.size() == 2)
            left = std::max(a[0], a[1]);
          else if (name == "abs" && a.size() == 1)
            left = std::abs(a[0]);
          else if (name == "floor" && a.size() == 1)
            left = std::floor(a[0]);
          else if (name == "ceil" && a.size() == 1)
            left = std::ceil(a[0]);
          else if (name == "round" && a.size() == 1)
            left = std::round(a[0]);
          else
            error = QCoreApplication::translate("KomaMimic", "未知の関数または引数です: ") + name;
        } else if (vars.contains(name))
          left = vars[name];
        else
          error = QCoreApplication::translate("KomaMimic", "未知の変数です: ") + name;
      }
    }
    const QVector<QPair<QString, int>> ops = {
        {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<=", 4},
        {">=", 4}, {"<", 4},  {">", 4},  {"+", 5},  {"-", 5},
        {"*", 6},  {"/", 6},  {"%", 6}};
    while (error.isEmpty()) {
      QString op;
      int priority = 0;
      for (const auto &candidate : ops)
        if (candidate.second >= minimum && take(candidate.first)) {
          op = candidate.first;
          priority = candidate.second;
          break;
        }
      if (op.isEmpty())
        break;
      const auto right = expr(priority + 1);
      if (op == "+")
        left += right;
      else if (op == "-")
        left -= right;
      else if (op == "*")
        left *= right;
      else if (op == "/" || op == "%") {
        if (right == 0)
          error = QCoreApplication::translate("KomaMimic", "ゼロ除算です。");
        else
          left = op == "/" ? left / right : std::fmod(left, right);
      } else if (op == "==")
        left = left == right;
      else if (op == "!=")
        left = left != right;
      else if (op == "<")
        left = left < right;
      else if (op == ">")
        left = left > right;
      else if (op == "<=")
        left = left <= right;
      else if (op == ">=")
        left = left >= right;
      else if (op == "&&")
        left = left && right;
      else if (op == "||")
        left = left || right;
      if (!std::isfinite(left))
        error = QCoreApplication::translate("KomaMimic", "計算結果が有限値ではありません。");
    }
    if (minimum == 0 && error.isEmpty() && take("?")) {
      const auto yes = expr();
      if (!take(":"))
        error = QCoreApplication::translate("KomaMimic", "条件式に : がありません。");
      const auto no = expr();
      left = left ? yes : no;
    }
    --depth;
    return left;
  }
};
} // namespace
ExpressionValue evaluateExpression(const QString &source,
                                   const QHash<QString, double> &variables) {
  if (source.size() > 4096)
    return {0, QCoreApplication::translate("KomaMimic", "式が長すぎます。")};
  Parser p;
  p.s = source;
  p.vars = variables;
  const auto v = p.expr();
  p.take(" ");
  if (p.error.isEmpty() && p.at != source.size())
    p.error = QCoreApplication::translate("KomaMimic", "未対応の式です。");
  return {v, p.error};
}
