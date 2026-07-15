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

#include "markdownpreview.h"

#include <QColor>
#include <QDomDocument>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLoggingCategory>
#include <QPainter>
#include <QPen>
#include <QProcess>
#include <QTextLayout>
#include <QTextOption>
#include <QVector>

Q_DECLARE_LOGGING_CATEGORY(OOXML_THUMBNAIL_LOG)

namespace
{
constexpr qsizetype maximumPreviewCharacters = 12000;
constexpr qsizetype maximumBlocks = 256;
constexpr int extractorTimeoutMs = 4000;

struct MarkdownLine {
    QString text;
    bool title = false;
    bool code = false;
};

QByteArray runCmark(const QString &filePath, QByteArray *standardError = nullptr)
{
    QProcess process;
    process.start(QStringLiteral("cmark"),
                  {QStringLiteral("--to"),
                   QStringLiteral("xml"),
                   QStringLiteral("--safe"),
                   QStringLiteral("--validate-utf8"),
                   QStringLiteral("--nobreaks"),
                   filePath});
    if (!process.waitForStarted()) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Failed to start cmark" << filePath;
        return {};
    }
    if (!process.waitForFinished(extractorTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        qCDebug(OOXML_THUMBNAIL_LOG) << "cmark timed out" << filePath;
        return {};
    }
    if (standardError) {
        *standardError = process.readAllStandardError();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "cmark failed" << filePath << process.exitCode();
        return {};
    }
    return process.readAllStandardOutput();
}

QString normalizedText(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.simplified();
}

QString collectText(const QDomNode &node)
{
    if (node.isText()) {
        return node.nodeValue();
    }

    QString result;
    const QDomNodeList children = node.childNodes();
    for (int i = 0; i < children.size(); ++i) {
        const QString childText = collectText(children.at(i));
        if (childText.isEmpty()) {
            continue;
        }
        if (!result.isEmpty()) {
            result += QLatin1Char(' ');
        }
        result += childText;
    }
    return normalizedText(result);
}

void appendLine(QVector<MarkdownLine> &lines, qsizetype &characterCount, QString text, bool title = false, bool code = false)
{
    text = normalizedText(text);
    if (text.isEmpty() || lines.size() >= maximumBlocks || characterCount >= maximumPreviewCharacters) {
        return;
    }
    if (text.size() > maximumPreviewCharacters - characterCount) {
        text.truncate(maximumPreviewCharacters - characterCount);
    }
    lines.append({text, title, code});
    characterCount += text.size();
}

void appendBlockLines(const QString &text, QVector<MarkdownLine> &lines, qsizetype &characterCount, bool title = false, bool code = false)
{
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    bool firstLine = true;
    for (const QString &rawLine : rawLines) {
        appendLine(lines, characterCount, rawLine, title && firstLine, code);
        firstLine = false;
        if (lines.size() >= maximumBlocks || characterCount >= maximumPreviewCharacters) {
            break;
        }
    }
}

QVector<MarkdownLine> parseMarkdownDocument(const QDomDocument &document)
{
    QVector<MarkdownLine> lines;
    lines.reserve(64);
    qsizetype characterCount = 0;
    bool titleAssigned = false;

    const QDomElement root = document.documentElement();
    const QDomNodeList nodes = root.childNodes();
    for (int i = 0; i < nodes.size(); ++i) {
        const QDomElement element = nodes.at(i).toElement();
        if (element.isNull()) {
            continue;
        }

        const QString tagName = element.tagName();
        if (tagName == QLatin1String("heading")) {
            const QString text = collectText(element);
            if (!text.isEmpty()) {
                appendLine(lines, characterCount, text, !titleAssigned, false);
                titleAssigned = true;
            }
        } else if (tagName == QLatin1String("paragraph")) {
            appendLine(lines, characterCount, collectText(element));
        } else if (tagName == QLatin1String("item")) {
            const QString text = collectText(element);
            appendLine(lines, characterCount, QStringLiteral("- %1").arg(text));
        } else if (tagName == QLatin1String("code_block")) {
            appendBlockLines(collectText(element), lines, characterCount, false, true);
        } else if (tagName == QLatin1String("block_quote")) {
            const QString text = collectText(element);
            appendLine(lines, characterCount, QStringLiteral("| %1").arg(text));
        }

        if (lines.size() >= maximumBlocks || characterCount >= maximumPreviewCharacters) {
            break;
        }
    }
    return lines;
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

QImage renderMarkdownDocument(const QVector<MarkdownLine> &lines, const QSize &targetSize)
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
    painter.fillRect(pageRect, QColor(250, 250, 250));

    const qreal contentMargin = qMin(qMax<qreal>(1.0, pageRect.width() * 0.075), qMin(pageRect.width(), pageRect.height()) * 0.25);
    const QRectF contentRect = pageRect.adjusted(contentMargin, contentMargin, -contentMargin, -contentMargin);
    const int bodyPixelSize = qBound(7, shortestSide / 30, 14);
    const int titlePixelSize = qBound(10, qRound(bodyPixelSize * 1.7), 26);
    const int codePixelSize = qBound(7, bodyPixelSize - 1, 13);
    qreal y = contentRect.top();

    for (const MarkdownLine &line : lines) {
        QFont font;
        if (line.code) {
            font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            font.setPixelSize(codePixelSize);
        } else {
            font.setPixelSize(line.title ? titlePixelSize : bodyPixelSize);
            font.setBold(line.title);
        }

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

        if (line.code) {
            QRectF blockRect(contentRect.left() - 2.0, y - 1.0, contentRect.width() + 4.0, lineHeight + 4.0);
            painter.fillRect(blockRect, QColor(242, 244, 247));
        }

        painter.save();
        painter.setClipRect(contentRect);
        painter.setPen(line.code ? QColor(66, 72, 84) : QColor(32, 36, 40));
        layout.draw(&painter, QPointF(contentRect.left(), y));
        painter.restore();

        y += lineHeight + qMax<qreal>(1.0, bodyPixelSize * (line.title ? 0.55 : 0.24));
        if (y >= contentRect.bottom()) {
            break;
        }
    }

    return image;
}
}

QImage renderMarkdownPreview(const QString &filePath, const QSize &targetSize)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Failed to open markdown file" << filePath;
        return {};
    }
    file.close();

    QByteArray standardError;
    const QByteArray xml = runCmark(filePath, &standardError);
    if (xml.isEmpty()) {
        if (!standardError.isEmpty()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "cmark stderr" << standardError;
        }
        return {};
    }

    QDomDocument document;
    QString parseError;
    int errorLine = 0;
    int errorColumn = 0;
    if (!document.setContent(xml, &parseError, &errorLine, &errorColumn)) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Failed to parse cmark XML" << parseError << errorLine << errorColumn;
        return {};
    }

    const QVector<MarkdownLine> lines = parseMarkdownDocument(document);
    if (lines.isEmpty()) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Markdown preview has no renderable content" << filePath;
        return {};
    }

    return addFormatWatermark(renderMarkdownDocument(lines, targetSize), QStringLiteral("MD"), QColor(29, 111, 180));
}
