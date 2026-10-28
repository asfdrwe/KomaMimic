// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QHash>
#include <QString>
struct ExpressionValue {
  double value = 0;
  QString error;
};
ExpressionValue
evaluateExpression(const QString &source,
                   const QHash<QString, double> &variables = {});
