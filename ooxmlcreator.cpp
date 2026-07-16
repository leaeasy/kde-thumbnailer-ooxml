/*
 *  This file is part of kde-thumbnailer-ooxml
 *  Copyright (C) 2012 Ni Hui <shuizhuyuanluo@126.com>
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

#include "ooxmlcreator.h"

#include "legacyofficepreview.h"
#include "markdownpreview.h"

#include <KArchiveFile>
#include <KArchiveDirectory>
#include <KPluginFactory>
#include <KZip>
#include <QDomDocument>
#include <QDomElement>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QLoggingCategory>
#include <QPainter>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QUrl>
#include <QVector>

#include <utility>

#include "libkowmf/WmfPainterBackend.h"

K_PLUGIN_CLASS_WITH_JSON(OOXmlCreator, "ooxmlthumbnail.json")
Q_LOGGING_CATEGORY(OOXML_THUMBNAIL_LOG, "kde.thumbnailer.ooxml")

namespace
{
constexpr auto thumbnailRelationship = "http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail";
constexpr auto officeRelationshipNamespace = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr auto packageRelationshipNamespace = "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr auto wordNamespace = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr auto spreadsheetNamespace = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr auto presentationNamespace = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr auto drawingNamespace = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr qint64 maximumXmlSize = 8 * 1024 * 1024;
constexpr qsizetype maximumPreviewCharacters = 12000;
constexpr qint64 maximumThumbnailPixels = 4096LL * 4096LL;
constexpr qsizetype maximumStyles = 4096;
constexpr qsizetype maximumParagraphs = 512;
constexpr qsizetype maximumRuns = 4096;

struct TextProperties {
    QString family;
    qreal pointSize = 0.0;
    QColor color;
    bool bold = false;
    bool italic = false;
    bool hasBold = false;
    bool hasItalic = false;
};

struct StyledRun {
    QString text;
    TextProperties properties;
};

struct StyledParagraph {
    QVector<StyledRun> runs;
    Qt::Alignment alignment = Qt::AlignLeft;
    bool title = false;
    bool heading = false;
};

struct StyledDocument {
    QVector<StyledParagraph> paragraphs;
    qreal basePointSize = 11.0;
};

struct WordStyle {
    QString basedOn;
    QString name;
    TextProperties properties;
};

void overlayProperties(TextProperties &base, const TextProperties &overlay)
{
    if (!overlay.family.isEmpty()) {
        base.family = overlay.family;
    }
    if (overlay.pointSize > 0.0) {
        base.pointSize = overlay.pointSize;
    }
    if (overlay.color.isValid()) {
        base.color = overlay.color;
    }
    if (overlay.hasBold) {
        base.bold = overlay.bold;
        base.hasBold = true;
    }
    if (overlay.hasItalic) {
        base.italic = overlay.italic;
        base.hasItalic = true;
    }
}

const KArchiveFile *archiveFile(const KZip &zip, const QString &path, qint64 maximumSize = maximumXmlSize)
{
    const KArchiveEntry *entry = zip.directory()->entry(path);
    if (!entry || !entry->isFile()) {
        return nullptr;
    }

    const auto *file = static_cast<const KArchiveFile *>(entry);
    return file->size() >= 0 && file->size() <= maximumSize ? file : nullptr;
}

bool parseXml(const QByteArray &data, QDomDocument &document)
{
    return bool(document.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing));
}

QString attribute(const QDomElement &element, const char *nameSpace, const char *name)
{
    return element.attributeNS(QLatin1String(nameSpace), QLatin1String(name));
}

QString elementText(const QDomElement &element, const char *nameSpace, const char *textElement)
{
    QString text;
    const QDomNodeList nodes = element.elementsByTagNameNS(QLatin1String(nameSpace), QLatin1String(textElement));
    for (qsizetype i = 0; i < nodes.size() && text.size() < maximumPreviewCharacters; ++i) {
        text += nodes.at(i).toElement().text();
    }
    return text.left(maximumPreviewCharacters);
}

QDomElement directChild(const QDomElement &parent, const char *nameSpace, const char *name)
{
    for (QDomNode node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const QDomElement element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == QLatin1String(nameSpace) && element.localName() == QLatin1String(name)) {
            return element;
        }
    }
    return {};
}

bool onOffValue(const QDomElement &element, const char *nameSpace)
{
    const QString value = attribute(element, nameSpace, "val").toLower();
    return value.isEmpty() || (value != QLatin1String("0") && value != QLatin1String("false") && value != QLatin1String("off"));
}

TextProperties wordRunProperties(const QDomElement &runProperties)
{
    TextProperties properties;
    if (runProperties.isNull()) {
        return properties;
    }

    const QDomElement fonts = directChild(runProperties, wordNamespace, "rFonts");
    if (!fonts.isNull()) {
        const QStringList candidates = {
            attribute(fonts, wordNamespace, "eastAsia"),
            attribute(fonts, wordNamespace, "ascii"),
            attribute(fonts, wordNamespace, "hAnsi"),
            attribute(fonts, wordNamespace, "cs"),
        };
        for (const QString &candidate : candidates) {
            if (!candidate.isEmpty()) {
                properties.family = candidate;
                break;
            }
        }
    }

    QDomElement size = directChild(runProperties, wordNamespace, "sz");
    if (size.isNull()) {
        size = directChild(runProperties, wordNamespace, "szCs");
    }
    if (!size.isNull()) {
        bool ok = false;
        const qreal halfPoints = attribute(size, wordNamespace, "val").toDouble(&ok);
        if (ok && halfPoints > 0.0 && halfPoints <= 2048.0) {
            properties.pointSize = halfPoints / 2.0;
        }
    }

    const QDomElement bold = directChild(runProperties, wordNamespace, "b");
    if (!bold.isNull()) {
        properties.hasBold = true;
        properties.bold = onOffValue(bold, wordNamespace);
    }
    const QDomElement italic = directChild(runProperties, wordNamespace, "i");
    if (!italic.isNull()) {
        properties.hasItalic = true;
        properties.italic = onOffValue(italic, wordNamespace);
    }
    const QDomElement color = directChild(runProperties, wordNamespace, "color");
    if (!color.isNull()) {
        const QString value = attribute(color, wordNamespace, "val");
        if (value.size() == 6 && value.compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0) {
            const QColor parsed(QLatin1Char('#') + value);
            if (parsed.isValid()) {
                properties.color = parsed;
            }
        }
    }
    return properties;
}

TextProperties resolvedWordStyle(const QString &styleId, const QHash<QString, WordStyle> &styles)
{
    QVector<const WordStyle *> chain;
    QString current = styleId;
    for (int depth = 0; depth < 16 && !current.isEmpty(); ++depth) {
        const auto it = styles.constFind(current);
        if (it == styles.cend()) {
            break;
        }
        chain.prepend(&it.value());
        if (it->basedOn == current) {
            break;
        }
        current = it->basedOn;
    }

    TextProperties properties;
    for (const WordStyle *style : chain) {
        overlayProperties(properties, style->properties);
    }
    return properties;
}

TextProperties spreadsheetFontProperties(const QDomElement &fontElement)
{
    TextProperties properties;
    const QDomElement name = directChild(fontElement, spreadsheetNamespace, "name");
    properties.family = name.attribute(QStringLiteral("val"));
    const QDomElement size = directChild(fontElement, spreadsheetNamespace, "sz");
    bool sizeOk = false;
    const qreal pointSize = size.attribute(QStringLiteral("val")).toDouble(&sizeOk);
    if (sizeOk && pointSize > 0.0 && pointSize <= 1024.0) {
        properties.pointSize = pointSize;
    }
    const QDomElement bold = directChild(fontElement, spreadsheetNamespace, "b");
    if (!bold.isNull()) {
        properties.hasBold = true;
        const QString value = bold.attribute(QStringLiteral("val")).toLower();
        properties.bold = value.isEmpty() || (value != QLatin1String("0") && value != QLatin1String("false"));
    }
    const QDomElement italic = directChild(fontElement, spreadsheetNamespace, "i");
    if (!italic.isNull()) {
        properties.hasItalic = true;
        const QString value = italic.attribute(QStringLiteral("val")).toLower();
        properties.italic = value.isEmpty() || (value != QLatin1String("0") && value != QLatin1String("false"));
    }
    const QDomElement color = directChild(fontElement, spreadsheetNamespace, "color");
    QString colorValue = color.attribute(QStringLiteral("rgb"));
    if (colorValue.size() == 8) {
        colorValue.remove(0, 2);
    }
    if (colorValue.size() == 6) {
        const QColor parsed(QLatin1Char('#') + colorValue);
        if (parsed.isValid()) {
            properties.color = parsed;
        }
    }
    return properties;
}

TextProperties drawingRunProperties(const QDomElement &runProperties)
{
    TextProperties properties;
    if (runProperties.isNull()) {
        return properties;
    }
    QDomElement font = directChild(runProperties, drawingNamespace, "ea");
    if (font.isNull()) {
        font = directChild(runProperties, drawingNamespace, "latin");
    }
    if (font.isNull()) {
        font = directChild(runProperties, drawingNamespace, "cs");
    }
    properties.family = font.attribute(QStringLiteral("typeface"));

    bool sizeOk = false;
    const qreal hundredthPoints = runProperties.attribute(QStringLiteral("sz")).toDouble(&sizeOk);
    if (sizeOk && hundredthPoints > 0.0 && hundredthPoints <= 102400.0) {
        properties.pointSize = hundredthPoints / 100.0;
    }
    if (runProperties.hasAttribute(QStringLiteral("b"))) {
        properties.hasBold = true;
        properties.bold = runProperties.attribute(QStringLiteral("b")) != QLatin1String("0");
    }
    if (runProperties.hasAttribute(QStringLiteral("i"))) {
        properties.hasItalic = true;
        properties.italic = runProperties.attribute(QStringLiteral("i")) != QLatin1String("0");
    }
    const QDomElement solidFill = directChild(runProperties, drawingNamespace, "solidFill");
    const QDomElement rgb = directChild(solidFill, drawingNamespace, "srgbClr");
    const QString colorValue = rgb.attribute(QStringLiteral("val"));
    if (colorValue.size() == 6) {
        const QColor parsed(QLatin1Char('#') + colorValue);
        if (parsed.isValid()) {
            properties.color = parsed;
        }
    }
    return properties;
}

QString wordRunText(const QDomElement &run)
{
    QString text;
    for (QDomNode node = run.firstChild(); !node.isNull() && text.size() < maximumPreviewCharacters; node = node.nextSibling()) {
        const QDomElement element = node.toElement();
        if (element.isNull() || element.namespaceURI() != QLatin1String(wordNamespace)) {
            continue;
        }
        if (element.localName() == QLatin1String("t")) {
            text += element.text();
        } else if (element.localName() == QLatin1String("tab")) {
            text += QLatin1Char('\t');
        } else if (element.localName() == QLatin1String("br") || element.localName() == QLatin1String("cr")) {
            text += QLatin1Char('\n');
        }
    }
    return text;
}

QString relationshipTarget(const KZip &zip, const QString &relationshipsPath, const QString &relationshipId)
{
    const KArchiveFile *file = archiveFile(zip, relationshipsPath);
    if (!file) {
        return {};
    }

    QDomDocument document;
    if (!parseXml(file->data(), document)) {
        return {};
    }

    const QDomNodeList relationships = document.elementsByTagNameNS(QLatin1String(packageRelationshipNamespace), QStringLiteral("Relationship"));
    for (qsizetype i = 0; i < relationships.size(); ++i) {
        const QDomElement relationship = relationships.at(i).toElement();
        if (relationship.attribute(QStringLiteral("Id")) == relationshipId
            && relationship.attribute(QStringLiteral("TargetMode")) != QLatin1String("External")) {
            return relationship.attribute(QStringLiteral("Target"));
        }
    }
    return {};
}

QString archivePathForTarget(const QString &target)
{
    const QUrl url = QUrl::fromEncoded(target.toUtf8(), QUrl::StrictMode);
    if (!url.isValid() || !url.scheme().isEmpty() || !url.authority().isEmpty() || url.hasQuery() || url.hasFragment()) {
        return {};
    }

    QString path = url.path(QUrl::FullyDecoded);
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }

    path = QDir::cleanPath(path);
    if (path.isEmpty() || path == QLatin1String(".") || path == QLatin1String("..") || path.startsWith(QLatin1String("../"))) {
        return {};
    }
    return path;
}

QString resolvedArchivePath(const QString &baseDirectory, const QString &target)
{
    const QString path = archivePathForTarget(target);
    if (path.isEmpty()) {
        return {};
    }

    if (target.startsWith(QLatin1Char('/'))) {
        return path;
    }
    return QDir::cleanPath(baseDirectory + QLatin1Char('/') + path);
}

QImage renderPage(const StyledDocument &document, const QSize &targetSize, qreal pageAspectRatio)
{
    if (document.paragraphs.isEmpty() || pageAspectRatio <= 0.0) {
        return {};
    }

    QImage image(targetSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const int shortestSide = qMin(targetSize.width(), targetSize.height());
    const int outerMargin = qMin(qMax(2, shortestSide / 32), qMax(0, (shortestSide - 1) / 2));
    QRectF available = QRectF(QPointF(outerMargin, outerMargin), QSizeF(targetSize.width() - outerMargin * 2, targetSize.height() - outerMargin * 2));
    QSizeF pageSize = available.size();
    if (pageSize.width() / pageSize.height() > pageAspectRatio) {
        pageSize.setWidth(pageSize.height() * pageAspectRatio);
    } else {
        pageSize.setHeight(pageSize.width() / pageAspectRatio);
    }
    QRectF pageRect(QPointF(), pageSize);
    pageRect.moveCenter(available.center());

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(pageRect.translated(1.5, 2.0), QColor(0, 0, 0, 45));
    painter.fillRect(pageRect, Qt::white);

    const qreal contentMargin = qMin(qMax<qreal>(1.0, pageRect.width() * 0.075), qMin(pageRect.width(), pageRect.height()) * 0.25);
    const QSizeF contentSize(pageRect.width() - contentMargin * 2, pageRect.height() - contentMargin * 2);
    painter.save();
    painter.translate(pageRect.topLeft() + QPointF(contentMargin, contentMargin));
    painter.setClipRect(QRectF(QPointF(), contentSize));

    const int bodyPixelSize = qBound(7, shortestSide / 30, 14);
    const qreal basePointSize = document.basePointSize > 0.0 ? document.basePointSize : 11.0;
    qreal y = 0.0;
    for (const StyledParagraph &paragraph : document.paragraphs) {
        QString text;
        QVector<QTextLayout::FormatRange> formats;
        for (const StyledRun &run : paragraph.runs) {
            if (run.text.isEmpty()) {
                continue;
            }
            QTextLayout::FormatRange range;
            range.start = text.size();
            range.length = run.text.size();
            const qreal sourceSize = run.properties.pointSize > 0.0 ? run.properties.pointSize : basePointSize;
            qreal scale = sourceSize / basePointSize;
            if (paragraph.title && scale < 1.7) {
                scale = 1.7;
            } else if (paragraph.heading && scale < 1.3) {
                scale = 1.3;
            }
            QFont font;
            if (!run.properties.family.isEmpty()) {
                font.setFamily(run.properties.family);
            }
            font.setPixelSize(qBound(6, qRound(bodyPixelSize * scale), 28));
            font.setBold(run.properties.hasBold ? run.properties.bold : paragraph.title || paragraph.heading);
            font.setItalic(run.properties.hasItalic && run.properties.italic);
            range.format.setFont(font);
            range.format.setForeground(run.properties.color.isValid() ? run.properties.color : QColor(32, 33, 36));
            formats.append(range);
            text += run.text;
        }
        if (text.trimmed().isEmpty()) {
            y += qMax<qreal>(2.0, bodyPixelSize * 0.5);
            continue;
        }

        QTextLayout layout(text);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(paragraph.alignment | Qt::AlignTop);
        layout.setTextOption(option);
        layout.setFormats(formats);
        layout.beginLayout();
        qreal paragraphHeight = 0.0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(contentSize.width());
            line.setPosition(QPointF(0.0, paragraphHeight));
            paragraphHeight += line.height();
            if (y + paragraphHeight >= contentSize.height()) {
                break;
            }
        }
        layout.endLayout();
        layout.draw(&painter, QPointF(0.0, y));
        y += paragraphHeight + qMax<qreal>(1.0, bodyPixelSize * (paragraph.title ? 0.55 : paragraph.heading ? 0.35 : 0.2));
        if (y >= contentSize.height()) {
            break;
        }
    }
    painter.restore();
    return image;
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

QImage embeddedThumbnail(const KZip &zip, const QSize &targetSize)
{
    const KArchiveFile *relsFile = archiveFile(zip, QStringLiteral("_rels/.rels"));
    if (!relsFile) {
        return {};
    }

    QDomDocument document;
    if (!parseXml(relsFile->data(), document)) {
        return {};
    }

    QString target;
    const QDomNodeList relationships = document.elementsByTagNameNS(QLatin1String(packageRelationshipNamespace), QStringLiteral("Relationship"));
    for (qsizetype i = 0; i < relationships.size(); ++i) {
        const QDomElement relationship = relationships.at(i).toElement();
        if (relationship.attribute(QStringLiteral("Type")) == QLatin1String(thumbnailRelationship)
            && relationship.attribute(QStringLiteral("TargetMode")) != QLatin1String("External")) {
            target = relationship.attribute(QStringLiteral("Target"));
            break;
        }
    }

    const QString archivePath = archivePathForTarget(target);
    const KArchiveFile *imageFile = archiveFile(zip, archivePath, 64 * 1024 * 1024);
    if (!imageFile) {
        return {};
    }

    QImage image;
    const QByteArray imageData = imageFile->data();
    if (archivePath.endsWith(QLatin1String(".wmf"), Qt::CaseInsensitive)) {
        image = QImage(targetSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        Libwmf::WmfPainterBackend wmf(&painter, image.size());
        if (!wmf.load(imageData) || !wmf.play()) {
            return {};
        }
    } else if (!image.loadFromData(imageData)) {
        return {};
    }

    if (image.width() > targetSize.width() || image.height() > targetSize.height()) {
        image = image.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

QImage renderWordPreview(const KZip &zip, const QSize &targetSize)
{
    const KArchiveFile *file = archiveFile(zip, QStringLiteral("word/document.xml"));
    if (!file) {
        return {};
    }

    QDomDocument document;
    if (!parseXml(file->data(), document)) {
        return {};
    }

    TextProperties defaults;
    QHash<QString, WordStyle> styles;
    if (const KArchiveFile *stylesFile = archiveFile(zip, QStringLiteral("word/styles.xml"))) {
        QDomDocument stylesDocument;
        if (parseXml(stylesFile->data(), stylesDocument)) {
            const QDomNodeList defaultsNodes = stylesDocument.elementsByTagNameNS(QLatin1String(wordNamespace), QStringLiteral("docDefaults"));
            if (!defaultsNodes.isEmpty()) {
                const QDomElement runDefaults = directChild(defaultsNodes.at(0).toElement(), wordNamespace, "rPrDefault");
                defaults = wordRunProperties(directChild(runDefaults, wordNamespace, "rPr"));
            }

            const QDomNodeList styleNodes = stylesDocument.elementsByTagNameNS(QLatin1String(wordNamespace), QStringLiteral("style"));
            const qsizetype styleCount = qMin<qsizetype>(styleNodes.size(), maximumStyles);
            for (qsizetype i = 0; i < styleCount; ++i) {
                const QDomElement styleElement = styleNodes.at(i).toElement();
                if (attribute(styleElement, wordNamespace, "type") != QLatin1String("paragraph")
                    && attribute(styleElement, wordNamespace, "type") != QLatin1String("character")) {
                    continue;
                }
                const QString styleId = attribute(styleElement, wordNamespace, "styleId");
                if (styleId.isEmpty()) {
                    continue;
                }
                WordStyle style;
                style.basedOn = attribute(directChild(styleElement, wordNamespace, "basedOn"), wordNamespace, "val");
                style.name = attribute(directChild(styleElement, wordNamespace, "name"), wordNamespace, "val");
                style.properties = wordRunProperties(directChild(styleElement, wordNamespace, "rPr"));
                styles.insert(styleId, style);
            }
        }
    }

    StyledDocument styledDocument;
    styledDocument.basePointSize = defaults.pointSize > 0.0 ? defaults.pointSize : 11.0;
    qsizetype characterCount = 0;
    qsizetype runCount = 0;
    const QDomNodeList paragraphs = document.elementsByTagNameNS(QLatin1String(wordNamespace), QStringLiteral("p"));
    const qsizetype paragraphCount = qMin<qsizetype>(paragraphs.size(), maximumParagraphs);
    for (qsizetype i = 0; i < paragraphCount && characterCount < maximumPreviewCharacters && runCount < maximumRuns; ++i) {
        const QDomElement paragraph = paragraphs.at(i).toElement();
        const QDomElement paragraphProperties = directChild(paragraph, wordNamespace, "pPr");
        const QString styleId = attribute(directChild(paragraphProperties, wordNamespace, "pStyle"), wordNamespace, "val");
        const WordStyle wordStyle = styles.value(styleId);
        const QString styleName = wordStyle.name.toLower();

        StyledParagraph styledParagraph;
        styledParagraph.heading = styleName.contains(QLatin1String("heading")) || styleName.contains(QLatin1String("标题"))
            || styleId.compare(QLatin1String("Heading1"), Qt::CaseInsensitive) == 0
            || styleId.compare(QLatin1String("Heading2"), Qt::CaseInsensitive) == 0 || styleId == QLatin1String("1") || styleId == QLatin1String("2");
        styledParagraph.title = styleName.contains(QLatin1String("title")) || styleName.contains(QLatin1String("题目"));
        const QString alignment = attribute(directChild(paragraphProperties, wordNamespace, "jc"), wordNamespace, "val");
        if (alignment == QLatin1String("center")) {
            styledParagraph.alignment = Qt::AlignHCenter;
        } else if (alignment == QLatin1String("right") || alignment == QLatin1String("end")) {
            styledParagraph.alignment = Qt::AlignRight;
        } else if (alignment == QLatin1String("both") || alignment == QLatin1String("distribute")) {
            styledParagraph.alignment = Qt::AlignJustify;
        }

        TextProperties paragraphDefaults = defaults;
        overlayProperties(paragraphDefaults, resolvedWordStyle(styleId, styles));
        overlayProperties(paragraphDefaults, wordRunProperties(directChild(paragraphProperties, wordNamespace, "rPr")));

        const QDomNodeList runs = paragraph.elementsByTagNameNS(QLatin1String(wordNamespace), QStringLiteral("r"));
        for (qsizetype runIndex = 0; runIndex < runs.size() && characterCount < maximumPreviewCharacters && runCount < maximumRuns; ++runIndex) {
            const QDomElement run = runs.at(runIndex).toElement();
            QString text = wordRunText(run);
            if (text.isEmpty()) {
                continue;
            }
            text = text.left(maximumPreviewCharacters - characterCount);
            TextProperties runProperties = paragraphDefaults;
            const QDomElement directRunProperties = directChild(run, wordNamespace, "rPr");
            const QString characterStyleId = attribute(directChild(directRunProperties, wordNamespace, "rStyle"), wordNamespace, "val");
            overlayProperties(runProperties, resolvedWordStyle(characterStyleId, styles));
            overlayProperties(runProperties, wordRunProperties(directRunProperties));
            styledParagraph.runs.append({text, runProperties});
            characterCount += text.size();
            ++runCount;
        }

        if (styledParagraph.runs.isEmpty()) {
            continue;
        }
        qreal largestPointSize = 0.0;
        for (const StyledRun &run : std::as_const(styledParagraph.runs)) {
            largestPointSize = qMax(largestPointSize, run.properties.pointSize);
        }
        if (!styledParagraph.title && !styledParagraph.heading && largestPointSize >= styledDocument.basePointSize * 1.5) {
            styledParagraph.title = true;
        } else if (!styledParagraph.title && !styledParagraph.heading && largestPointSize >= styledDocument.basePointSize * 1.2) {
            styledParagraph.heading = true;
        }
        styledDocument.paragraphs.append(styledParagraph);
    }

    qreal aspectRatio = 11906.0 / 16838.0;
    const QDomNodeList pageSizes = document.elementsByTagNameNS(QLatin1String(wordNamespace), QStringLiteral("pgSz"));
    if (!pageSizes.isEmpty()) {
        const QDomElement pageSize = pageSizes.at(pageSizes.size() - 1).toElement();
        bool widthOk = false;
        bool heightOk = false;
        const qreal width = attribute(pageSize, wordNamespace, "w").toDouble(&widthOk);
        const qreal height = attribute(pageSize, wordNamespace, "h").toDouble(&heightOk);
        if (widthOk && heightOk && width > 0.0 && height > 0.0) {
            aspectRatio = width / height;
        }
    }
    return renderPage(styledDocument, targetSize, aspectRatio);
}

QString firstRelatedPart(const KZip &zip,
                         const QString &documentPath,
                         const QString &relationshipsPath,
                         const char *elementNamespace,
                         const char *elementName,
                         const char *idAttributeNamespace,
                         const char *idAttributeName)
{
    const KArchiveFile *file = archiveFile(zip, documentPath);
    if (!file) {
        return {};
    }

    QDomDocument document;
    if (!parseXml(file->data(), document)) {
        return {};
    }
    const QDomNodeList elements = document.elementsByTagNameNS(QLatin1String(elementNamespace), QLatin1String(elementName));
    if (elements.isEmpty()) {
        return {};
    }

    const QString relationshipId = attribute(elements.at(0).toElement(), idAttributeNamespace, idAttributeName);
    const QString target = relationshipTarget(zip, relationshipsPath, relationshipId);
    return resolvedArchivePath(QFileInfo(documentPath).path(), target);
}

QImage renderSpreadsheetPreview(const KZip &zip, const QSize &targetSize)
{
    QString worksheetPath = firstRelatedPart(zip,
                                             QStringLiteral("xl/workbook.xml"),
                                             QStringLiteral("xl/_rels/workbook.xml.rels"),
                                             spreadsheetNamespace,
                                             "sheet",
                                             officeRelationshipNamespace,
                                             "id");
    if (worksheetPath.isEmpty()) {
        worksheetPath = QStringLiteral("xl/worksheets/sheet1.xml");
    }

    QStringList sharedStrings;
    if (const KArchiveFile *stringsFile = archiveFile(zip, QStringLiteral("xl/sharedStrings.xml"))) {
        QDomDocument stringsDocument;
        if (parseXml(stringsFile->data(), stringsDocument)) {
            const QDomNodeList items = stringsDocument.elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("si"));
            const qsizetype count = qMin<qsizetype>(items.size(), 10000);
            sharedStrings.reserve(count);
            for (qsizetype i = 0; i < count; ++i) {
                sharedStrings.append(elementText(items.at(i).toElement(), spreadsheetNamespace, "t"));
            }
        }
    }

    const KArchiveFile *worksheetFile = archiveFile(zip, worksheetPath);
    if (!worksheetFile) {
        return {};
    }
    QDomDocument worksheet;
    if (!parseXml(worksheetFile->data(), worksheet)) {
        return {};
    }

    QVector<TextProperties> fonts;
    QVector<int> cellFontIds;
    if (const KArchiveFile *stylesFile = archiveFile(zip, QStringLiteral("xl/styles.xml"))) {
        QDomDocument stylesDocument;
        if (parseXml(stylesFile->data(), stylesDocument)) {
            const QDomNodeList fontCollections = stylesDocument.elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("fonts"));
            if (!fontCollections.isEmpty()) {
                const QDomNodeList fontNodes = fontCollections.at(0).toElement().elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("font"));
                const qsizetype fontCount = qMin<qsizetype>(fontNodes.size(), maximumStyles);
                fonts.reserve(fontCount);
                for (qsizetype i = 0; i < fontCount; ++i) {
                    fonts.append(spreadsheetFontProperties(fontNodes.at(i).toElement()));
                }
            }
            const QDomNodeList xfsCollections = stylesDocument.elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("cellXfs"));
            if (!xfsCollections.isEmpty()) {
                const QDomNodeList xfs = xfsCollections.at(0).toElement().elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("xf"));
                const qsizetype xfCount = qMin<qsizetype>(xfs.size(), maximumStyles);
                cellFontIds.reserve(xfCount);
                for (qsizetype i = 0; i < xfCount; ++i) {
                    bool ok = false;
                    const int fontId = xfs.at(i).toElement().attribute(QStringLiteral("fontId")).toInt(&ok);
                    cellFontIds.append(ok ? fontId : 0);
                }
            }
        }
    }

    StyledDocument styledDocument;
    if (!fonts.isEmpty() && fonts.at(0).pointSize > 0.0) {
        styledDocument.basePointSize = fonts.at(0).pointSize;
    }
    qsizetype characterCount = 0;
    const QDomNodeList rows = worksheet.elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("row"));
    const qsizetype rowCount = qMin<qsizetype>(rows.size(), 24);
    for (qsizetype rowIndex = 0; rowIndex < rowCount && characterCount < maximumPreviewCharacters; ++rowIndex) {
        StyledParagraph paragraph;
        const QDomNodeList cells = rows.at(rowIndex).toElement().elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("c"));
        const qsizetype cellCount = qMin<qsizetype>(cells.size(), 10);
        for (qsizetype cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            const QDomElement cell = cells.at(cellIndex).toElement();
            QString value;
            if (cell.attribute(QStringLiteral("t")) == QLatin1String("inlineStr")) {
                value = elementText(cell, spreadsheetNamespace, "t");
            } else {
                const QDomNodeList values = cell.elementsByTagNameNS(QLatin1String(spreadsheetNamespace), QStringLiteral("v"));
                if (!values.isEmpty()) {
                    value = values.at(0).toElement().text();
                }
                if (cell.attribute(QStringLiteral("t")) == QLatin1String("s")) {
                    bool ok = false;
                    const int index = value.toInt(&ok);
                    value = ok && index >= 0 && index < sharedStrings.size() ? sharedStrings.at(index) : QString();
                }
            }
            value = value.simplified().left(80);
            characterCount += value.size();
            if (value.isEmpty()) {
                continue;
            }
            if (!paragraph.runs.isEmpty()) {
                paragraph.runs.append({QStringLiteral("  |  "), {}});
            }
            bool styleOk = false;
            const int styleIndex = cell.attribute(QStringLiteral("s")).toInt(&styleOk);
            TextProperties properties;
            if (styleOk && styleIndex >= 0 && styleIndex < cellFontIds.size()) {
                const int fontId = cellFontIds.at(styleIndex);
                if (fontId >= 0 && fontId < fonts.size()) {
                    properties = fonts.at(fontId);
                }
            } else if (!fonts.isEmpty()) {
                properties = fonts.at(0);
            }
            paragraph.runs.append({value, properties});
        }
        if (!paragraph.runs.isEmpty()) {
            qreal largestPointSize = 0.0;
            bool containsBoldText = false;
            for (const StyledRun &run : std::as_const(paragraph.runs)) {
                largestPointSize = qMax(largestPointSize, run.properties.pointSize);
                containsBoldText = containsBoldText || (run.properties.hasBold && run.properties.bold);
            }
            paragraph.heading = containsBoldText || largestPointSize >= styledDocument.basePointSize * 1.2;
            styledDocument.paragraphs.append(paragraph);
        }
    }
    return renderPage(styledDocument, targetSize, 1.414);
}

QImage renderPresentationPreview(const KZip &zip, const QSize &targetSize)
{
    QString slidePath = firstRelatedPart(zip,
                                         QStringLiteral("ppt/presentation.xml"),
                                         QStringLiteral("ppt/_rels/presentation.xml.rels"),
                                         presentationNamespace,
                                         "sldId",
                                         officeRelationshipNamespace,
                                         "id");
    if (slidePath.isEmpty()) {
        slidePath = QStringLiteral("ppt/slides/slide1.xml");
    }

    const KArchiveFile *slideFile = archiveFile(zip, slidePath);
    if (!slideFile) {
        return {};
    }
    QDomDocument slide;
    if (!parseXml(slideFile->data(), slide)) {
        return {};
    }

    StyledDocument styledDocument;
    styledDocument.basePointSize = 18.0;
    bool hasTitle = false;
    qsizetype characterCount = 0;
    const QDomNodeList paragraphs = slide.elementsByTagNameNS(QLatin1String(drawingNamespace), QStringLiteral("p"));
    for (qsizetype i = 0; i < paragraphs.size() && characterCount < maximumPreviewCharacters; ++i) {
        const QDomElement paragraphElement = paragraphs.at(i).toElement();
        StyledParagraph paragraph;
        const QDomElement shape = paragraphElement.parentNode().parentNode().toElement();
        const QDomNodeList placeholders = shape.elementsByTagNameNS(QLatin1String(presentationNamespace), QStringLiteral("ph"));
        const QString placeholderType = placeholders.isEmpty() ? QString() : placeholders.at(0).toElement().attribute(QStringLiteral("type"));
        paragraph.title = placeholderType == QLatin1String("title") || placeholderType == QLatin1String("ctrTitle");
        const QDomElement paragraphProperties = directChild(paragraphElement, drawingNamespace, "pPr");
        const QString alignment = paragraphProperties.attribute(QStringLiteral("algn"));
        if (alignment == QLatin1String("ctr")) {
            paragraph.alignment = Qt::AlignHCenter;
        } else if (alignment == QLatin1String("r")) {
            paragraph.alignment = Qt::AlignRight;
        } else if (alignment == QLatin1String("just")) {
            paragraph.alignment = Qt::AlignJustify;
        }
        TextProperties paragraphDefaults = drawingRunProperties(directChild(paragraphProperties, drawingNamespace, "defRPr"));
        for (QDomNode node = paragraphElement.firstChild(); !node.isNull() && characterCount < maximumPreviewCharacters; node = node.nextSibling()) {
            const QDomElement run = node.toElement();
            if (run.isNull() || run.namespaceURI() != QLatin1String(drawingNamespace) || run.localName() != QLatin1String("r")) {
                continue;
            }
            const QDomElement textElement = directChild(run, drawingNamespace, "t");
            QString text = textElement.text();
            if (text.isEmpty()) {
                continue;
            }
            text = text.left(maximumPreviewCharacters - characterCount);
            TextProperties properties = paragraphDefaults;
            overlayProperties(properties, drawingRunProperties(directChild(run, drawingNamespace, "rPr")));
            paragraph.runs.append({text, properties});
            characterCount += text.size();
        }
        if (paragraph.runs.isEmpty()) {
            continue;
        }
        qreal largestPointSize = 0.0;
        for (const StyledRun &run : std::as_const(paragraph.runs)) {
            largestPointSize = qMax(largestPointSize, run.properties.pointSize);
        }
        if (!paragraph.title && !hasTitle && (styledDocument.paragraphs.isEmpty() || largestPointSize >= styledDocument.basePointSize * 1.4)) {
            paragraph.title = true;
        }
        hasTitle = true;
        styledDocument.paragraphs.append(paragraph);
    }

    qreal aspectRatio = 16.0 / 9.0;
    if (const KArchiveFile *presentationFile = archiveFile(zip, QStringLiteral("ppt/presentation.xml"))) {
        QDomDocument presentation;
        if (parseXml(presentationFile->data(), presentation)) {
            const QDomNodeList slideSizes = presentation.elementsByTagNameNS(QLatin1String(presentationNamespace), QStringLiteral("sldSz"));
            if (!slideSizes.isEmpty()) {
                bool widthOk = false;
                bool heightOk = false;
                const QDomElement slideSize = slideSizes.at(0).toElement();
                const qreal width = slideSize.attribute(QStringLiteral("cx")).toDouble(&widthOk);
                const qreal height = slideSize.attribute(QStringLiteral("cy")).toDouble(&heightOk);
                if (widthOk && heightOk && width > 0.0 && height > 0.0) {
                    aspectRatio = width / height;
                }
            }
        }
    }
    return renderPage(styledDocument, targetSize, aspectRatio);
}

QImage fallbackPreview(const KZip &zip, const KIO::ThumbnailRequest &request)
{
    const QString mimeType = request.mimeType();
    if (mimeType == QLatin1String("application/vnd.openxmlformats-officedocument.wordprocessingml.document")
        || mimeType == QLatin1String("application/wps-office.docx")) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Rendering DOCX fallback preview";
        return addFormatWatermark(renderWordPreview(zip, request.targetSize()), QStringLiteral("DOCX"), QColor(43, 87, 154));
    }
    if (mimeType == QLatin1String("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
        || mimeType == QLatin1String("application/wps-office.xlsx")) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Rendering XLSX fallback preview";
        return addFormatWatermark(renderSpreadsheetPreview(zip, request.targetSize()), QStringLiteral("XLSX"), QColor(33, 115, 70));
    }
    if (mimeType == QLatin1String("application/vnd.openxmlformats-officedocument.presentationml.presentation")
        || mimeType == QLatin1String("application/wps-office.pptx")) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Rendering PPTX fallback preview";
        return addFormatWatermark(renderPresentationPreview(zip, request.targetSize()), QStringLiteral("PPTX"), QColor(196, 63, 32));
    }
    qCDebug(OOXML_THUMBNAIL_LOG) << "Unsupported fallback MIME type" << mimeType;
    return {};
}
}

OOXmlCreator::OOXmlCreator(QObject *parent, const QVariantList &args)
    : KIO::ThumbnailCreator(parent, args)
{
}

KIO::ThumbnailResult OOXmlCreator::create(const KIO::ThumbnailRequest &request)
{
    const QUrl url = request.url();
    const QSize targetSize = request.targetSize();
    const QString mimeType = request.mimeType();
    qCDebug(OOXML_THUMBNAIL_LOG) << "Thumbnail request" << url << request.mimeType() << targetSize << "DPR" << request.devicePixelRatio();

    const qint64 pixelCount = qint64(targetSize.width()) * qint64(targetSize.height());
    if (!url.isLocalFile() || !targetSize.isValid() || targetSize.isEmpty() || pixelCount > maximumThumbnailPixels) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Rejected invalid thumbnail request";
        return KIO::ThumbnailResult::fail();
    }

    if (mimeType == QLatin1String("application/msword")
        || mimeType == QLatin1String("application/wps-office.doc")) {
        QImage image = renderLegacyDocPreview(url.toLocalFile(), targetSize);
        if (image.isNull()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "DOC thumbnail generation failed";
            return KIO::ThumbnailResult::fail();
        }
        image.setDevicePixelRatio(request.devicePixelRatio());
        return KIO::ThumbnailResult::pass(image);
    }

    if (mimeType == QLatin1String("text/markdown")
        || mimeType == QLatin1String("text/x-markdown")) {
        QImage image = renderMarkdownPreview(url.toLocalFile(), targetSize);
        if (image.isNull()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "Markdown thumbnail generation failed";
            return KIO::ThumbnailResult::fail();
        }
        image.setDevicePixelRatio(request.devicePixelRatio());
        return KIO::ThumbnailResult::pass(image);
    }

    if (mimeType == QLatin1String("text/html")
        || mimeType == QLatin1String("application/xhtml+xml")) {
        QImage image = renderHtmlPreview(url.toLocalFile(), targetSize);
        if (image.isNull()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "HTML thumbnail generation failed";
            return KIO::ThumbnailResult::fail();
        }
        image.setDevicePixelRatio(request.devicePixelRatio());
        return KIO::ThumbnailResult::pass(image);
    }

    if (mimeType == QLatin1String("application/vnd.ms-powerpoint")
        || mimeType == QLatin1String("application/wps-office.ppt")) {
        QImage image = renderLegacyPptPreview(url.toLocalFile(), targetSize);
        if (image.isNull()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "PPT thumbnail generation failed";
            return KIO::ThumbnailResult::fail();
        }
        image.setDevicePixelRatio(request.devicePixelRatio());
        return KIO::ThumbnailResult::pass(image);
    }

    if (mimeType == QLatin1String("application/vnd.ms-excel")
        || mimeType == QLatin1String("application/wps-office.xls")) {
        QImage image = renderLegacyXlsPreview(url.toLocalFile(), targetSize);
        if (image.isNull()) {
            qCDebug(OOXML_THUMBNAIL_LOG) << "XLS thumbnail generation failed";
            return KIO::ThumbnailResult::fail();
        }
        image.setDevicePixelRatio(request.devicePixelRatio());
        return KIO::ThumbnailResult::pass(image);
    }

    KZip zip(url.toLocalFile());
    if (!zip.open(QIODevice::ReadOnly)) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Failed to open OOXML ZIP" << url.toLocalFile();
        return KIO::ThumbnailResult::fail();
    }

    QImage image = embeddedThumbnail(zip, targetSize);
    if (image.isNull()) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "No usable embedded thumbnail, trying fallback renderer";
        image = fallbackPreview(zip, request);
    } else {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Using embedded thumbnail" << image.size();
    }
    if (image.isNull()) {
        qCDebug(OOXML_THUMBNAIL_LOG) << "Thumbnail generation failed";
        return KIO::ThumbnailResult::fail();
    }

    image.setDevicePixelRatio(request.devicePixelRatio());
    qCDebug(OOXML_THUMBNAIL_LOG) << "Thumbnail generation succeeded" << image.size();
    return KIO::ThumbnailResult::pass(image);
}

#include "ooxmlcreator.moc"
