﻿// .h include
#include "jqqmlimagemanage.h"

// Qt lib import
#include <QDebug>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQuickView>
#include <QQuickWindow>
#include <QQuickImageProvider>
#include <QQuickTextureFactory>
#include <QFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QTime>
#include <QMap>
#include <QSharedPointer>
#include <QMutex>
#include <QtConcurrent>

#include <iostream>

#pragma pack(push)
#pragma pack(8)
struct ImageInformationHead
{
    qint32 imageWidth;
    qint32 imageHeight;
    qint32 imageFormat;
    qint32 imageColorCount;
    qint32 imageFileSize;
    qint64 imageLastModified;
};
#pragma pack(pop)

struct PreloadCacheData
{
    QSharedPointer< QMutex > mutexForPreload_; // 预读取的时候会lock，读取完毕立即unlock和释放这个mutex
    QByteArray headData;
    QByteArray imageData;
};

// JQQmlImageTextureFactory
class JQQmlImageTextureFactory: public QQuickTextureFactory
{
public:
    JQQmlImageTextureFactory(const QString &id):
        id_( id )
    {
        if ( id.isEmpty() ) { return; }

        QString imageFilePath;

        if ( id.startsWith( "qrc:/" ) )
        {
            imageFilePath = ":/";
            imageFilePath += id.mid( 5 );
        }
        else if ( id.startsWith( "file:/" ) )
        {
            imageFilePath = QUrl( id ).toLocalFile();
        }
        if ( imageFilePath.isEmpty() )
        {
            qDebug() << "JQQmlImageTextureFactory::JQQmlImageTextureFactory: id error:" << id;
            return;
        }

        const auto &&jqicFilePath = JQQmlImageManage::jqicFilePath( imageFilePath );
        QFile jqicFile( jqicFilePath );

        ImageInformationHead imageInformationHead;

        if ( preloadCacheDatas_.contains( jqicFilePath ) )
        {
            // 有匹配的预加载数据，直接使用预加载数据

            const auto &preloadCacheData = preloadCacheDatas_[ jqicFilePath ];
            auto mutex = preloadCacheData.mutexForPreload_;
            if ( mutex )
            {
                // 如果已经进入了预加载列表但是实际上没有读取完，那么进行等待

                mutex->lock();
                mutex->unlock();
            }

            memcpy( &imageInformationHead, preloadCacheData.headData.constData(), sizeof( ImageInformationHead ) );

            buffer_ = preloadCacheData.imageData;
            image_ = QImage(
                        ( const uchar * )buffer_.constData(),
                        imageInformationHead.imageWidth,
                        imageInformationHead.imageHeight,
                        ( QImage::Format )imageInformationHead.imageFormat
                    );

            JQQmlImageManage::jqQmlImageManage()->recordImageFilePath( imageFilePath );
        }
        else if ( jqicFile.exists() && ( jqicFile.size() >= ( qint64 )sizeof( ImageInformationHead ) ) )
        {
            // 在本地发现缓存，直接加载缓存数据

            if ( !jqicFile.open( QIODevice::ReadOnly ) )
            {
                qDebug() << "open file error:" << jqicFilePath;
                return;
            }

            jqicFile.read( (char *)&imageInformationHead, sizeof( ImageInformationHead ) );

            buffer_ = jqicFile.readAll();
            image_ = QImage(
                        ( const uchar * )buffer_.constData(),
                        imageInformationHead.imageWidth,
                        imageInformationHead.imageHeight,
                        ( QImage::Format )imageInformationHead.imageFormat
                    );
            image_.setColorCount( imageInformationHead.imageColorCount );

            JQQmlImageManage::jqQmlImageManage()->recordImageFilePath( imageFilePath );
        }
        else
        {
            // 在本地没有发现图片缓存，那么重新加载图片

            QTime timeForLoad;

            timeForLoad.start();
            image_.load( imageFilePath );
            const auto &&loadElapsed = timeForLoad.elapsed();

            if ( image_.isNull() )
            {
                qDebug() << "JQQmlImageTextureFactory::JQQmlImageTextureFactory: load error:" << imageFilePath;
                return;
            }

            // 加载很快的图片不进行缓存
            if ( loadElapsed < 3 ) { return; }

            // 内容过少的图片不进行缓存
            if ( image_.byteCount() <= ( 40 * 40 * 4 ) ) { return; }

            if ( ( image_.format() == QImage::Format_Mono ) ||
                 ( image_.format() == QImage::Format_Indexed8 ))
            {
                if ( image_.hasAlphaChannel() )
                {
                    image_ = image_.convertToFormat( QImage::Format_ARGB32 );
                }
                else
                {
                    image_ = image_.convertToFormat( QImage::Format_RGB888 );
                }
            }

            const auto &&jqicFileInfo = QFileInfo( jqicFilePath );

            imageInformationHead.imageWidth = image_.width();
            imageInformationHead.imageHeight = image_.height();
            imageInformationHead.imageFormat = image_.format();
            imageInformationHead.imageColorCount = image_.colorCount();
            imageInformationHead.imageFileSize = jqicFileInfo.size();
            imageInformationHead.imageLastModified = jqicFileInfo.lastModified().toMSecsSinceEpoch();

            const auto &&headData = QByteArray( (const char *)&imageInformationHead, sizeof( ImageInformationHead ) );
            const auto &&imageData = QByteArray( (const char *)image_.constBits(), image_.byteCount() );

            // 到新线程去存储缓存文件，不影响主线程
            QtConcurrent::run(
                        [
                            jqicFilePath,
                            headData,
                            imageData
                        ]()
            {
                QFile jqicFile( jqicFilePath );

                if ( !jqicFile.open( QIODevice::WriteOnly ) )
                {
                    qDebug() << "open file error:" << jqicFilePath;
                    return;
                }
                jqicFile.resize( headData.size() + imageData.size() );

                jqicFile.write( headData );
                jqicFile.write( imageData );

                jqicFile.waitForBytesWritten( 30 * 1000 );
            } );

            JQQmlImageManage::jqQmlImageManage()->recordImageFilePath( imageFilePath );
        }
    }

    ~JQQmlImageTextureFactory() = default;

    QSGTexture *createTexture(QQuickWindow *window) const
    {
        return window->createTextureFromImage( image_ );
    }

    int textureByteCount() const
    {
        return image_.byteCount();
    }

    QSize textureSize() const
    {
        return image_.size();
    }

    static bool preload(const QString &jqicFilePath)
    {
        if ( !QFileInfo::exists( jqicFilePath ) ) { return false; }

        if ( preloadCacheDatas_.contains( jqicFilePath ) ) { return false; }

        auto &preloadCacheData = preloadCacheDatas_[ jqicFilePath ];
        preloadCacheData.mutexForPreload_.reset( new QMutex );
        preloadCacheData.mutexForPreload_->lock();

        // 到新线程去加载，不影响主线程
        QtConcurrent::run(
                    [
                        jqicFilePath,
                        &preloadCacheData
                    ]()
        {
            QFile jqicFile( jqicFilePath );

            if ( !jqicFile.exists() || ( jqicFile.size() < ( qint64 )sizeof( ImageInformationHead ) ) )
            {
                preloadCacheData.mutexForPreload_->unlock();
                preloadCacheData.mutexForPreload_.clear();
                return;
            }

            if ( !jqicFile.open( QIODevice::ReadOnly ) )
            {
                qDebug() << "open file error:" << jqicFilePath;

                preloadCacheData.mutexForPreload_->unlock();
                preloadCacheData.mutexForPreload_.clear();

                return;
            }

            preloadCacheData.headData = jqicFile.read( sizeof( ImageInformationHead ) );
            preloadCacheData.imageData = jqicFile.readAll();

            preloadCacheData.mutexForPreload_->unlock();
            preloadCacheData.mutexForPreload_.clear();
        } );

        return true;
    }

private:
    static QMap< QString, PreloadCacheData > preloadCacheDatas_; // jqicFilePath -> PreloadCacheData; 预读取数据的容器

    QString id_;
    QImage image_;
    QByteArray buffer_;
};

QMap< QString, PreloadCacheData > JQQmlImageTextureFactory::preloadCacheDatas_;

// JQQmlImageImageProvider
class JQQmlImageImageProvider: public QQuickImageProvider
{
public:
    JQQmlImageImageProvider():
        QQuickImageProvider( QQmlImageProviderBase::Texture )
    { }

    ~JQQmlImageImageProvider() = default;

    QQuickTextureFactory *requestTexture(const QString &id, QSize *, const QSize &)
    {
        return new JQQmlImageTextureFactory( id );
    }
};

// JQQmlImageManage
QPointer< QQmlApplicationEngine > JQQmlImageManage::qmlApplicationEngine_;
QPointer< QQuickView > JQQmlImageManage::quickView_;
QPointer< JQQmlImageManage > JQQmlImageManage::jqQmlImageManage_;

JQQmlImageManage::JQQmlImageManage():
    mutexForAutoPreloadImage_( new QMutex ),
    listForAutoPreloadImage_( new QStringList )
{
    jqQmlImageManage_ = this;

    if ( !qmlApplicationEngine_.isNull() )
    {
        qmlApplicationEngine_->addImageProvider( "JQQmlImage", new JQQmlImageImageProvider );
    }
    else if ( !quickView_.isNull() )
    {
        quickView_->engine()->addImageProvider( "JQQmlImage", new JQQmlImageImageProvider );
    }
    else
    {
        qDebug() << "JQQmlImageManage::JQQmlImageManage: error";
    }
}

JQQmlImageManage::~JQQmlImageManage()
{
    jqQmlImageManage_ = nullptr;

    mutexForAutoPreloadImage_->lock();
    saveAutoPreloadImageFileListToFile( *listForAutoPreloadImage_ );
    mutexForAutoPreloadImage_->unlock();
}

void JQQmlImageManage::initialize(QQmlApplicationEngine *qmlApplicationEngine)
{
    qmlApplicationEngine->addImportPath( ":/JQQmlImageQml/" );
    qmlApplicationEngine_ = qmlApplicationEngine;
}

void JQQmlImageManage::initialize(QQuickView *quickView)
{
    quickView->engine()->addImportPath( ":/JQQmlImageQml/" );
    quickView_ = quickView;
}

bool JQQmlImageManage::preload(const QString &imageFilePath)
{
    return JQQmlImageTextureFactory::preload( jqicFilePath( imageFilePath ) );
}

void JQQmlImageManage::autoPreload()
{
    const auto &&list = readAutoPreloadImageFileListToFile();

    for ( const auto &filePath: list )
    {
        preload( filePath );
    }
}

bool JQQmlImageManage::clearAllCache()
{
    return QDir( jqicPath() ).removeRecursively();
}

QString JQQmlImageManage::jqicPath()
{
    if ( !qApp )
    {
        qDebug() << "JQQmlImageManage::jqicPath: error, qApp is null";
        return { };
    }

    const auto &&cacheLocation = QStandardPaths::writableLocation( QStandardPaths::CacheLocation );
    if ( cacheLocation.isEmpty() )
    {
        qDebug() << "JQQmlImageManage::jqicPath: error, CacheLocation is empty";
        return { };
    }

    const auto &&buf = QString( "%1/jqqmlimagecache" ).arg( cacheLocation );
    if ( !QFileInfo( buf ).exists() )
    {
        if ( !QDir().mkpath( buf ) )
        {
            qDebug() << "JQQmlImageManage::jqicPath: mkpath error:" << buf;
            return { };
        }
    }

    return buf;
}

QString JQQmlImageManage::jqicFilePath(const QString &imageFilePath)
{
    const auto &&imageFileInfo = QFileInfo( imageFilePath );
    if ( !imageFileInfo.isFile() ) { return { }; }

    QByteArray sumString;

    sumString += imageFilePath;
    sumString += "|";
    sumString += QByteArray::number( imageFileInfo.lastModified().toMSecsSinceEpoch() );

    const auto &&md5String = QCryptographicHash::hash( sumString, QCryptographicHash::Md5 ).toHex();

    return QString( "%1/jqqmlimagecache/%2.jqic" ).
            arg( QStandardPaths::writableLocation( QStandardPaths::CacheLocation ), md5String.constData() );
}

void JQQmlImageManage::recordImageFilePath(const QString &imageFilePath)
{
    if ( listForAutoPreloadImage_->size() >= 10 ) { return; }

    mutexForAutoPreloadImage_->lock();
    listForAutoPreloadImage_->push_back( imageFilePath );
    mutexForAutoPreloadImage_->unlock();
}

void JQQmlImageManage::saveAutoPreloadImageFileListToFile(const QStringList &imageFilePathList)
{
    auto file = autoPreloadImageFile();
    if ( !file->open( QIODevice::WriteOnly ) )
    {
        qDebug() << "open file error:" << file->fileName();
        return;
    }

    file->write( imageFilePathList.join( "\n" ).toUtf8() );
    file->waitForBytesWritten( 30 * 1000 );
}

QStringList JQQmlImageManage::readAutoPreloadImageFileListToFile()
{
    auto file = autoPreloadImageFile();

    if ( !file->exists() ) { return { }; }

    if ( !file->open( QIODevice::ReadOnly ) )
    {
        qDebug() << "open file error:" << file->fileName();
        return { };
    }

    const auto &&rawList = file->readAll().split( '\n' );
    QStringList reply;

    for ( const auto &filePath: rawList )
    {
        reply.push_back( QString::fromUtf8( filePath ) );
    }

    return reply;
}

QSharedPointer< QFile > JQQmlImageManage::autoPreloadImageFile()
{
    return QSharedPointer< QFile >( new QFile( QString( "%1/autopreloadlist" ).arg( JQQmlImageManage::jqicPath() ) ) );
}
