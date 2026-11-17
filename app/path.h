#pragma once

#include <QString>
#include <QFileInfo>

class Path
{
public:
    static QString getLogDir();
    static QString getBoxArtCacheDir();
    static QString getQmlCacheDir();

    static QByteArray readDataFile(QString fileName);
    static void writeCacheFile(QString fileName, QByteArray data);
    static void deleteCacheFile(QString fileName);
    static QFileInfo getCacheFileInfo(QString fileName);

    // Only safe to use directly for Qt classes
    static QString getDataFilePath(QString fileName);

    static void initialize(bool portable);

    // GpuVM: logs and caches all under one folder (one a Low integrity process can write)
    static void initializeIn(const QString& directory);

private:
    static QString s_CacheDir;
    static QString s_LogDir;
    static QString s_BoxArtCacheDir;
    static QString s_QmlCacheDir;
};
