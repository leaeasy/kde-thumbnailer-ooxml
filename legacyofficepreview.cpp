/*
 *  This file is part of kde-thumbnailer-ooxml
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of
 *  the License or (at your option) version 3 or any later version
 *  accepted by the membership of KDE e.V. (or its successor approved
 *  by the membership of KDE e.V.), which shall act as a proxy
 *  defined in Section 14 of version 3 of the license.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "legacyofficepreview.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QLoggingCategory>
#include <QPainter>
#include <QPen>
#include <QProcess>
#include <QStringConverter>
#include <QStringList>
#include <QTextLayout>
#include <QTextOption>
#include <QVector>

Q_DECLARE_LOGGING_CATEGORY(OOXML_THUMBNAIL_LOG)

namespace
{
constexpr qsizetype maximumPreviewCharacters = 12000;
constexpr qsizetype maximumParagraphs = 512;
constexpr qsizetype maximumRows = 32;
constexpr qsizetype maximumColumns = 8;
constexpr int extractorTimeoutMs = 4000;

struct TextLine {
    QString text;
    bool title = false;
};

QByteArray runExtractor(const QString &program,
                        const QStringList &arguments,
                        const QString &filePath,
                        QByteArray *standardError = nullptr)
{
    QProcess process;
    process.start(program, arguments + QStringList{filePath});
    if (!process.waitForStarted()) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Failed to start extractor" << program << filePath;
        return {};
    }
    if (!process.waitForFinished(extractorTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        qCDebug(OOXML_THUMBNAIL_LOG) << "Extractor timed out" << program << filePath;
        return {};
    }
    if (standardError) {
        *standardError = process.readAllStandardError();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Extractor failed" << program << filePath << process.exitCode();
        return {};
    }
    return process.readAllStandardOutput();
}

QString decodedOutput(const QByteArray &data)
{
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8.decode(data);
    if (!text.isEmpty() || data.isEmpty()) {
        return text;
    }
    return QString::fromLocal8Bit(data);
}

QString normalizedLine(QString line)
{
    line.replace(QLatin1Char('\r'), QLatin1Char(' '));
    line.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return line.simplified();
}

QImage addFormatWatermark(QImage image, const QString &label, const QColor &color)
{
    if (image.isNull() || label.isEmpty()) {
        return image;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(qBound(7, qMin(image.width(), image.height()) / 14, 22));
    painter.setFont(font);

    const QFontMetrics metrics(font);
    const int horizontalPadding = qMax(4, metrics.height() / 3);
    const int verticalPadding = qMax(2, metrics.height() / 6);
    const int outerMargin = qMax(3, qMin(image.width(), image.height()) / 40);
    const QSize badgeSize(metrics.horizontalAdvance(label) + horizontalPadding * 2,
                          metrics.height() + verticalPadding * 2);
    QRect badgeRect(QPoint(), badgeSize);
    badgeRect.moveBottomRight(QPoint(image.width() - outerMargin, image.height() - outerMargin));

    QColor background = color;
    background.setAlpha(220);
    QColor border = color.darker(135);
    border.setAlpha(245);
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(background);
    painter.drawRoundedRect(badgeRect, verticalPadding * 1.5, verticalPadding * 1.5);

    painter.setPen(QColor(255, 255, 255, 220));
    painter.drawText(badgeRect, Qt::AlignCenter, label);
    return image;
}

QImage renderTextDocument(const QVector<TextLine> &lines, const QSize &targetSize)
{
    if (lines.isEmpty() || !targetSize.isValid() || targetSize.isEmpty()) {
        return {};
    }

    QImage image(targetSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const int shortestSide = qMin(targetSize.width(), targetSize.height());
    const int outerMargin = qMin(qMax(2, shortestSide / 32), qMax(0, (shortestSide - 1) / 2));
    QRectF pageRect(QPointF(outerMargin, outerMargin), QSizeF(targetSize.width() - outerMargin * 2, targetSize.height() - outerMargin * 2));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(pageRect.translated(1.5, 2.0), QColor(0, 0, 0, 45));
    painter.fillRect(pageRect, Qt::white);

    const qreal contentMargin = qMin(qMax<qreal>(1.0, pageRect.width() * 0.075), qMin(pageRect.width(), pageRect.height()) * 0.25);
    const QRectF contentRect = pageRect.adjusted(contentMargin, contentMargin, -contentMargin, -contentMargin);
    const int bodyPixelSize = qBound(7, shortestSide / 30, 14);
    qreal y = contentRect.top();

    for (const TextLine &line : lines) {
        if (line.text.isEmpty()) {
            y += qMax<qreal>(2.0, bodyPixelSize * 0.45);
            continue;
        }

        QFont font;
        font.setPixelSize(line.title ? qBound(9, qRound(bodyPixelSize * 1.65), 26) : bodyPixelSize);
        font.setBold(line.title);

        QTextLayout layout(line.text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(Qt::AlignLeft | Qt::AlignTop);
        layout.setTextOption(option);
        layout.beginLayout();
        qreal lineHeight = 0.0;
        while (true) {
            QTextLine textLine = layout.createLine();
            if (!textLine.isValid()) {
                break;
            }
            textLine.setLineWidth(contentRect.width());
            textLine.setPosition(QPointF(0.0, lineHeight));
            lineHeight += textLine.height();
            if (y + lineHeight >= contentRect.bottom()) {
                break;
            }
        }
        layout.endLayout();

        painter.save();
        painter.setClipRect(contentRect);
        layout.draw(&painter, QPointF(contentRect.left(), y));
        painter.restore();

        y += lineHeight + qMax<qreal>(1.0, bodyPixelSize * (line.title ? 0.55 : 0.22));
        if (y >= contentRect.bottom()) {
            break;
        }
    }

    return image;
}

QImage renderCsvTable(const QVector<QStringList> &rows, const QSize &targetSize)
{
    if (rows.isEmpty() || !targetSize.isValid() || targetSize.isEmpty()) {
        return {};
    }

    QImage image(targetSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const int shortestSide = qMin(targetSize.width(), targetSize.height());
    const int outerMargin = qMin(qMax(2, shortestSide / 32), qMax(0, (shortestSide - 1) / 2));
    QRectF pageRect(QPointF(outerMargin, outerMargin), QSizeF(targetSize.width() - outerMargin * 2, targetSize.height() - outerMargin * 2));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(pageRect.translated(1.5, 2.0), QColor(0, 0, 0, 45));
    painter.fillRect(pageRect, Qt::white);

    const qreal contentMargin = qMin(qMax<qreal>(1.0, pageRect.width() * 0.06), qMin(pageRect.width(), pageRect.height()) * 0.2);
    const QRectF tableRect = pageRect.adjusted(contentMargin, contentMargin, -contentMargin, -contentMargin);
    const int rowCount = rows.size();
    const int columnCount = rows.constFirst().size();
    if (rowCount <= 0 || columnCount <= 0) {
        return {};
    }

    const qreal rowHeight = tableRect.height() / rowCount;
    const qreal columnWidth = tableRect.width() / columnCount;
    const int fontPixelSize = qBound(7, shortestSide / 34, 13);
    QFont font;
    font.setPixelSize(fontPixelSize);
    painter.setFont(font);

    for (int row = 0; row < rowCount; ++row) {
        for (int column = 0; column < columnCount; ++column) {
            QRectF cellRect(tableRect.left() + columnWidth * column,
                            tableRect.top() + rowHeight * row,
                            columnWidth,
                            rowHeight);
            const bool headerRow = row == 0;
            painter.fillRect(cellRect, headerRow ? QColor(236, 245, 239) : (row % 2 == 0 ? QColor(251, 252, 251) : QColor(245, 247, 245)));
            painter.setPen(QPen(QColor(204, 214, 206), 1.0));
            painter.drawRect(cellRect);

            painter.setPen(headerRow ? QColor(26, 84, 50) : QColor(44, 48, 44));
            QRectF textRect = cellRect.adjusted(4.0, 2.0, -4.0, -2.0);
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, rows.at(row).value(column));
        }
    }

    return image;
}

QVector<TextLine> parseDocLines(const QString &text)
{
    QVector<TextLine> lines;
    lines.reserve(qMin<qsizetype>(maximumParagraphs, text.count(QLatin1Char('\n')) + 1));

    qsizetype characterCount = 0;
    bool titleAssigned = false;
    bool previousWasBlank = false;
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : rawLines) {
        QString line = normalizedLine(rawLine);
        if (line.isEmpty()) {
            if (!previousWasBlank && !lines.isEmpty()) {
                lines.append(TextLine{});
                previousWasBlank = true;
            }
            continue;
        }

        if (characterCount >= maximumPreviewCharacters || lines.size() >= maximumParagraphs) {
            break;
        }

        if (line.size() > maximumPreviewCharacters - characterCount) {
            line.truncate(maximumPreviewCharacters - characterCount);
        }
        const bool isTitle = !titleAssigned && line.size() <= 120;
        lines.append({line, isTitle});
        titleAssigned = titleAssigned || isTitle;
        previousWasBlank = false;
        characterCount += line.size();
    }
    return lines;
}

QStringList parseCsvRow(const QString &line)
{
    QStringList cells;
    QString current;
    bool inQuotes = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                current += ch;
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
            continue;
        }
        if (ch == QLatin1Char(',') && !inQuotes) {
            cells.append(current.simplified());
            current.clear();
            continue;
        }
        current += ch;
    }
    cells.append(current.simplified());
    if (cells.size() > maximumColumns) {
        cells = cells.mid(0, maximumColumns);
    }
    return cells;
}

QVector<QStringList> parseCsvRows(const QString &text)
{
    QVector<QStringList> rows;
    rows.reserve(maximumRows);

    qsizetype characterCount = 0;
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : rawLines) {
        if (rows.size() >= maximumRows || characterCount >= maximumPreviewCharacters) {
            break;
        }
        QString line = rawLine;
        line.remove(QLatin1Char('\r'));
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QStringList cells = parseCsvRow(line);
        while (cells.size() < maximumColumns) {
            cells.append(QString());
        }
        rows.append(cells);
        characterCount += line.size();
    }
    return rows;
}
}

QImage renderLegacyDocPreview(const QString &filePath, const QSize &targetSize)
{
    QByteArray standardError;
    const QByteArray output = runExtractor(QStringLiteral("catdoc"), {}, filePath, &standardError);
    if (output.isEmpty()) {
        if (!standardError.isEmpty()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "catdoc stderr" << standardError;
        }
        return {};
    }

    const QVector<TextLine> lines = parseDocLines(decodedOutput(output));
    if (lines.isEmpty()) {
        return {};
    }
    return addFormatWatermark(renderTextDocument(lines, targetSize), QStringLiteral("DOC"), QColor(43, 87, 154));
}

QImage renderLegacyPptPreview(const QString &filePath, const QSize &targetSize)
{
    QByteArray standardError;
    const QByteArray output = runExtractor(QStringLiteral("catppt"), {}, filePath, &standardError);
    if (output.isEmpty()) {
        if (!standardError.isEmpty()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "catppt stderr" << standardError;
        }
        return {};
    }

    const QVector<TextLine> lines = parseDocLines(decodedOutput(output));
    if (lines.isEmpty()) {
        return {};
    }
    return addFormatWatermark(renderTextDocument(lines, targetSize), QStringLiteral("PPT"), QColor(196, 63, 32));
}

QImage renderLegacyXlsPreview(const QString &filePath, const QSize &targetSize)
{
    QByteArray standardError;
    const QByteArray output = runExtractor(QStringLiteral("xls2csv"), {}, filePath, &standardError);
    if (output.isEmpty()) {
        if (!standardError.isEmpty()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "xls2csv stderr" << standardError;
        }
        return {};
    }

    const QVector<QStringList> rows = parseCsvRows(decodedOutput(output));
    if (rows.isEmpty()) {
        return {};
    }
    return addFormatWatermark(renderCsvTable(rows, targetSize), QStringLiteral("XLS"), QColor(33, 115, 70));
}
