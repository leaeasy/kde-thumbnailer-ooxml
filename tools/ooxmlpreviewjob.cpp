#include <KFileItem>
#include <KIO/PreviewJob>
#include <KPluginMetaData>

#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTextStream output(stdout);
    QTextStream error(stderr);

    if (argc != 3) {
        error << "Usage: " << argv[0] << " INPUT OUTPUT.png\n";
        return 2;
    }

    const QFileInfo input(QString::fromLocal8Bit(argv[1]));
    const QString outputPath = QString::fromLocal8Bit(argv[2]);
    if (!input.isFile()) {
        error << "Input is not a readable file: " << input.absoluteFilePath() << '\n';
        return 2;
    }

    const KFileItem item(QUrl::fromLocalFile(input.absoluteFilePath()));
    const QStringList enabledPlugins{QStringLiteral("ooxmlthumbnail")};
    output << "Available thumbnail plugins:\n";
    for (const KPluginMetaData &plugin : KIO::PreviewJob::availableThumbnailerPlugins()) {
        if (plugin.pluginId() == QLatin1String("ooxmlthumbnail") || plugin.supportsMimeType(item.mimetype())) {
            output << "  id=" << plugin.pluginId() << " file=" << plugin.fileName() << " name=" << plugin.name()
                   << " enabled=" << (enabledPlugins.contains(plugin.pluginId()) ? "true" : "false") << '\n';
        }
    }
    output << "Input MIME type: " << item.mimetype() << '\n';
    output.flush();

    auto *job = KIO::filePreview(KFileItemList{item}, QSize(256, 256), &enabledPlugins);
    job->setScaleType(KIO::PreviewJob::ScaleType::Scaled);

    int exitCode = 1;
    QObject::connect(job, &KIO::PreviewJob::generated, &application, [&](const KFileItem &, const QImage &image) {
        if (!image.isNull() && image.save(outputPath, "PNG")) {
            output << "Generated " << image.width() << 'x' << image.height() << " preview: " << outputPath << '\n';
            exitCode = 0;
        } else {
            error << "Preview was generated but could not be saved\n";
        }
    });
    QObject::connect(job, &KIO::PreviewJob::failed, &application, [&](const KFileItem &) {
        error << "KIO::PreviewJob failed for " << input.absoluteFilePath() << '\n';
    });
    QObject::connect(job, &KJob::finished, &application, [&]() {
        if (job->error()) {
            error << "Preview job error " << job->error() << ": " << job->errorString() << '\n';
        }
        application.exit(exitCode);
    });

    QTimer::singleShot(15000, &application, [&]() {
        error << "Timed out waiting for KIO::PreviewJob\n";
        application.exit(124);
    });
    job->start();
    return application.exec();
}
