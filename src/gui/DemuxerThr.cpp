#include <DemuxerThr.hpp>

#include <PlayClass.hpp>
#include <AVThread.hpp>
#include <Writer.hpp>
#include <Main.hpp>

#include <SubsDec.hpp>
#include <Demuxer.hpp>
#include <Decoder.hpp>
#include <Reader.hpp>

#include <Functions.hpp>
using Functions::Url;
using Functions::gettime;
using Functions::filePath;
using Functions::fileName;
using Functions::sizeString;

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>

#include <math.h>

static inline bool getCurrentPlaying( int stream, const QList< StreamInfo * > &streamsInfo, const StreamInfo *streamInfo )
{
	return ( stream > -1 && streamsInfo.count() > stream ) && streamsInfo[ stream ] == streamInfo;
}
static inline QString getWriterName( AVThread *avThr )
{
	QString decName;
	if ( avThr )
	{
		decName = avThr->dec->name();
		if ( !decName.isEmpty() )
			decName += ", ";
	}
	return ( avThr && avThr->writer ) ? " - <i>" + decName + avThr->writer->name() + "</i>" : QString();
}

static inline QString getCoverFile( const QString &title, const QString &artist, const QString &album )
{
	return QMPlay2Core.getSettingsDir() + "Covers/" + QCryptographicHash::hash( ( album.isEmpty() ? title.toUtf8() : album.toUtf8() ) + artist.toUtf8(), QCryptographicHash::Md5 ).toHex();
}

/**/

DemuxerThr::DemuxerThr( PlayClass &playC ) :
	playC( playC ),
	hasCover( false )
{
	demuxerReady = br = err = false;
	convertAddressReader = NULL;
	demuxer = NULL;
	url = playC.url;

	connect( this, SIGNAL( stopVADec() ), this, SLOT( stopVADecSlot() ) );
}

QByteArray DemuxerThr::getCoverFromStream() const
{
	return demuxer ? demuxer->image( true ) : QByteArray();
}

void DemuxerThr::loadImage()
{
	if ( demuxerReady )
	{
		const QByteArray demuxerImage = demuxer->image();
		QImage img = QImage::fromData( demuxerImage );
		if ( !img.isNull() )
			emit QMPlay2Core.coverDataFromMediaFile( demuxerImage );
		else
		{
			if ( img.isNull() && QMPlay2Core.getSettings().getBool( "ShowDirCovers" ) ) //Ładowanie okładki z katalogu
			{
				const QString directory = filePath( url.mid( 7 ) );
				if ( directory != "://" )
					foreach ( QString cover, QDir( directory ).entryList( QStringList() << "cover" << "cover.*" << "folder" << "folder.*", QDir::Files ) )
					{
						QString coverPath = directory + cover;
						img = QImage( coverPath );
						if ( !img.isNull() )
						{
							emit QMPlay2Core.coverFile( coverPath );
							break;
						}
					}
			}
			if ( img.isNull() && !artist.isEmpty() && ( !title.isEmpty() || !album.isEmpty() ) ) //Ładowanie okładki z cache
			{
				QString coverPath = getCoverFile( title, artist, album );
				img = QImage( coverPath );
				if ( !img.isNull() )
					emit QMPlay2Core.coverFile( coverPath );
			}
		}
		hasCover = !img.isNull();
		emit playC.updateImage( img );
	}
}

void DemuxerThr::stop()
{
	br = true;
	abortMutex.lock();
	if ( convertAddressReader )
		convertAddressReader->abort();
	if ( demuxer )
		demuxer->abort();
	abortMutex.unlock();
}
void DemuxerThr::end()
{
	bool endMutexLocked = false;

	stopVAMutex.lock();
	if ( currentThread() != qApp->thread() )
	{
		endMutex.lock(); //Zablokuje główny wątek do czasu zniszczenia demuxer'a
		endMutexLocked = true;
		emit stopVADec(); //to wykonuje się w głównym wątku
		stopVAMutex.lock(); //i czeka na koniec wykonywania
		stopVAMutex.unlock();
	}
	else //wywołane z głównego wątku
		stopVADecSlot();

	delete demuxer;
	demuxer = NULL;

	if ( endMutexLocked )
		endMutex.unlock(); //Jeżeli był zablokowany, odblokuje mutex
}

bool DemuxerThr::load( bool canEmitInfo )
{
	playC.loadMutex.lock();
	emit load( demuxer ); //to wykonuje się w głównym wątku
	playC.loadMutex.lock(); //i czeka na koniec wykonywania
	playC.loadMutex.unlock();
	if ( canEmitInfo )
		emitInfo();
	return playC.audioStream >= 0 || playC.videoStream >= 0;
}

void DemuxerThr::run()
{
	emit playC.chText( tr( "Otwieranie" ) );
	emit playC.setCurrentPlaying();

	emit QMPlay2Core.busyCursor();
	Functions::getDataIfHasPluginPrefix( url, &url, &name, NULL, &convertAddressReader, &abortMutex );
	emit QMPlay2Core.restoreCursor();

	Demuxer::create( url, demuxer, br, &abortMutex );
	if ( !demuxer || br )
	{
		if ( !br && !demuxer )
		{
			QMPlay2Core.logError( tr( "Nie można otworzyć" ) + ": " + url.remove( "file://" ) );
			emit playC.updateCurrentEntry( QString(), -1 );
			err = true;
		}
		return end();
	}

	QStringList filter;
	filter << "*.ass" << "*.ssa";
	foreach ( QString ext, SubsDec::extensions() )
		filter << "*." + ext;
	foreach ( StreamInfo *streamInfo, demuxer->streamsInfo() )
		if ( streamInfo->type == QMPLAY2_TYPE_VIDEO ) //napisów szukam tylko wtedy, jeżeli jest strumień wideo
		{
			QString directory = filePath( url.mid( 7 ) );
			QString fName = fileName( url, false ).replace( '_', ' ' );
			foreach ( QString subsFile, QDir( directory ).entryList( filter, QDir::Files ) )
			{
				if ( fileName( subsFile, false ).replace( '_', ' ' ).contains( fName, Qt::CaseInsensitive ) )
				{
					QString fileSubsUrl = Url( directory + subsFile );
					if ( !playC.fileSubsList.contains( fileSubsUrl ) )
						playC.fileSubsList += fileSubsUrl;
				}
			}
			break;
		}

	err = !load( false );
	if ( err || br )
		return end();

	if ( playC.videoStream > -1 )
		playC.frame_last_delay = 1.0 / demuxer->streamsInfo()[ playC.videoStream ]->FPS;

	/* ReplayGain */
	float gain_db = 0.0f, peak = 1.0f;
	if ( !QMPlay2Core.getSettings().getBool( "ReplayGain/Enabled" ) || !demuxer->getReplayGain( QMPlay2Core.getSettings().getBool( "ReplayGain/Album" ), gain_db, peak ) )
		playC.replayGain = 1.0;
	else
	{
		playC.replayGain = pow( 10.0, gain_db / 20.0 ) * pow( 10.0, QMPlay2Core.getSettings().getDouble( "ReplayGain/Preamp" ) / 20.0 );
		if ( QMPlay2Core.getSettings().getBool( "ReplayGain/PreventClipping" ) && peak * playC.replayGain > 1.0 )
			playC.replayGain = 1.0 / peak;
	}

	bool unknownLength = demuxer->length() < 0.0;
	if ( unknownLength )
	{
		playIfBuffered = 0.0;
		updateBufferedSeconds = false;
	}

	emit playC.updateLength( unknownLength ? 0.0 : demuxer->length() );
	emit playC.chText( tr( "Odtwarzanie" ) );
	emit playC.playStateChanged( true );

	demuxerReady = true;

	updateCoverAndPlaying();

	connect( &QMPlay2Core, SIGNAL( updateCover( const QString &, const QString &, const QString &, const QByteArray & ) ), this, SLOT( updateCover( const QString &, const QString &, const QString &, const QByteArray & ) ) );

	const bool localStream = demuxer->localStream();
	const int MIN_BUF_SIZE = demuxer->dontUseBuffer() ? 1 : ( localStream ? minBuffSizeLocal : minBuffSizeNetwork );
	bool paused = false, demuxerPaused = false;
	double time = localStream ? 0.0 : gettime();
	qint64 buffered = 0, last_buffered = 0;
	double wfd_t = playIfBuffered > 0.25 ? 0.25 : playIfBuffered;
	double bufferedTime = 0.0;
	int vS, aS;

	setPriority( QThread::HighPriority );
	while ( !br )
	{
		AVThread *aThr = ( AVThread * )playC.aThr, *vThr = ( AVThread * )playC.vThr;

		if ( playC.seekTo >= 0 || playC.seekTo == -2 )
		{
			emit playC.chText( tr( "Przewijanie" ) );
			emit playC.updateBufferedSeconds( 0 );
			playC.canUpdatePos = false;

			bool noSeekInBuffer = false;
			if ( playC.seekTo == -2 ) //po otwarciu lub zmianie strumienia audio, wideo lub napisów
			{
				playC.seekTo = playC.pos;
				noSeekInBuffer = true;
			}

			const bool backwards = playC.seekTo < ( int )playC.pos;
			bool mustSeek = true, flush = false, aLocked = false, vLocked = false;

			//przewijanie do przodu na strumieniu sieciowym, szuka czy skok do zbuforowanego fragmentu i usuwa niepotrzebne paczki
			if ( !backwards && !localStream && !noSeekInBuffer )
			{
				playC.vPackets.lock();
				playC.aPackets.lock();
				playC.sPackets.lock();
				if
				(
					( playC.vPackets.packetCount() || playC.aPackets.packetCount() ) &&
					playC.vPackets.clipTo( playC.seekTo ) && playC.aPackets.clipTo( playC.seekTo ) && playC.sPackets.clipTo( playC.seekTo )
				)
				{
					flush = true;
					mustSeek = false;
					if ( !( bufferedTime = playC.vPackets.duration() ) )
						bufferedTime = playC.aPackets.duration();
					emit playC.updateBufferedSeconds( bufferedTime );
					if ( aThr )
						aLocked = aThr->lock();
					if ( vThr )
						vLocked = vThr->lock();
				}
				playC.vPackets.unlock();
				playC.aPackets.unlock();
				playC.sPackets.unlock();
			}

			if ( mustSeek && demuxer->seek( playC.seekTo, backwards ) )
				flush = true;

			if ( flush )
			{
				playC.endOfStream = false;
				if ( mustSeek )
					clearBuffers();
				else
					playC.flushAssEvents();
				if ( !aLocked && aThr )
					aLocked = aThr->lock();
				if ( !vLocked && vThr )
					vLocked = vThr->lock();
				playC.skipAudioFrame = playC.audio_current_pts = 0.0;
				playC.flushVideo = playC.flushAudio = true;
				if ( aLocked )
					aThr->unlock();
				if ( vLocked )
					vThr->unlock();
			}

			playC.canUpdatePos = true;
			playC.seekTo = -1;
			if ( !playC.paused )
				emit playC.chText( tr( "Odtwarzanie" ) );
			else
				playC.paused = false;
		}

		err = ( aThr && aThr->writer && !aThr->writer->readyWrite() ) || ( vThr && vThr->writer && !vThr->writer->readyWrite() );
		if ( br || err )
			break;

		if ( playC.paused )
		{
			if ( !paused )
			{
				paused = true;
				emit playC.chText( tr( "Pauza" ) );
				emit playC.playStateChanged( false );
				playC.emptyBufferCond.wakeAll();
			}
		}
		else if ( paused )
		{
			paused = demuxerPaused = false;
			emit playC.chText( tr( "Odtwarzanie" ) );
			emit playC.playStateChanged( true );
			playC.emptyBufferCond.wakeAll();
		}

		bool updateBuffered = localStream ? false : ( gettime() - time >= ( playC.waitForData ? wfd_t : 1.25 ) );
		getAVBuffersSize( vS, aS, ( updateBuffered || playC.waitForData ) ? &buffered : NULL, &bufferedTime );
		if ( playC.endOfStream && !vS && !aS && canBreak( aThr, vThr ) )
			break;
		if ( updateBuffered )
		{
			if ( last_buffered != buffered )
			{
				if ( updateBufferedSeconds )
					emit playC.updateBufferedSeconds( round( bufferedTime ) );
				emit playC.updateBuffered( last_buffered = buffered, bufferedTime );
				if ( demuxer->metadataChanged() )
					updateCoverAndPlaying();
			}
			time = gettime();
		}
		else if ( localStream && demuxer->metadataChanged() )
			updateCoverAndPlaying();

		if ( !localStream && !playC.waitForData && !playC.endOfStream && playIfBuffered > 0.0 && emptyBuffers( vS, aS ) )
			playC.waitForData = true;
		else if
		(
			playC.waitForData &&
			(
				playC.endOfStream                       ||
				bufferedPackets( vS, aS, MIN_BUF_SIZE ) ||
				( bufferedTime >= playIfBuffered )      ||
				( !bufferedTime && bufferedPackets( vS, aS, 1 ) )
			)
		)
		{
			playC.waitForData = false;
			if ( !paused )
				playC.emptyBufferCond.wakeAll();
		}

		if ( playC.endOfStream || bufferedPackets( vS, aS, MIN_BUF_SIZE ) )
		{
			if ( paused && !demuxerPaused )
			{
				demuxerPaused = true;
				demuxer->pause();
			}

			bool loadError = false;
			while ( !playC.fullBufferB )
			{
				if ( qApp->hasPendingEvents() )
					qApp->processEvents();
				else
				{
					msleep( 15 );
					if ( mustReloadStreams() && !load() )
					{
						loadError = true;
						break;
					}
					if ( playC.seekTo == -2 )
						break;
				}
			}
			if ( loadError )
				break;
			playC.fullBufferB = false;
			continue;
		}

		Packet packet;
		int streamIdx = -1;
		if ( demuxer->read( packet.data, streamIdx, packet.ts, packet.duration ) )
		{
			qApp->processEvents();

			if ( mustReloadStreams() && !load() )
				break;

			if ( streamIdx < 0 || playC.seekTo == -2 )
				continue;

			if ( streamIdx == playC.audioStream )
				playC.aPackets.enqueue( packet );
			else if ( streamIdx == playC.videoStream )
				playC.vPackets.enqueue( packet );
			else if ( streamIdx == playC.subtitlesStream )
				playC.sPackets.enqueue( packet );

			if ( !paused && !playC.waitForData )
				playC.emptyBufferCond.wakeAll();
		}
		else
		{
			getAVBuffersSize( vS, aS );
			if ( vS || aS || !canBreak( aThr, vThr ) )
			{
				playC.endOfStream = true;
				if ( !localStream )
					time = gettime() - 1.25; //zapewni, że updateBuffered będzie na "true"
			}
			else
				break;
		}
	}

	emit QMPlay2Core.updatePlaying( false, title, artist, album, demuxer->length(), false );

	playC.endOfStream = playC.canUpdatePos = false; //to musi tu być!
	end();
}

void DemuxerThr::updateCoverAndPlaying()
{
	foreach ( QMPlay2Tag tag, demuxer->tags() ) //wczytywanie tytuły, artysty i albumu
	{
		const QMPlay2Tags tagID = StreamInfo::getTag( tag.first );
		switch ( tagID )
		{
			case QMPLAY2_TAG_TITLE:
				title = tag.second;
				break;
			case QMPLAY2_TAG_ARTIST:
				artist = tag.second;
				break;
			case QMPLAY2_TAG_ALBUM:
				album = tag.second;
				break;
			default:
				break;
		}
	}
	const bool showCovers = QMPlay2Core.getSettings().getBool( "ShowCovers" );
	if ( showCovers )
		loadImage();
	emitInfo();
	emit QMPlay2Core.updatePlaying( true, title, artist, album, demuxer->length(), showCovers && !hasCover );
}

static void printOtherInfo( const QList< QMPlay2Tag > &other_info, QString &str )
{
	foreach ( QMPlay2Tag tag, other_info )
		if ( !tag.second.isEmpty() )
			str += "<li><b>" + StreamInfo::getTagName( tag.first ).toLower() + ":</b> " + tag.second + "</li>";
}
void DemuxerThr::addSubtitleStream( bool currentPlaying, QString &subtitlesStreams, int i, int subtitlesStreamCount, const QString &streamName, const QString &codecName, const QString &title, const QList< QMPlay2Tag > &other_info )
{
	subtitlesStreams += "<ul style='margin-top: 0px; margin-bottom: 0px;'>";
	if ( currentPlaying )
		subtitlesStreams += "<u>";
	else
		subtitlesStreams += "<a style='text-decoration: none; color: black;' href='stream:" + streamName + QString::number( i ) + "'>";
	subtitlesStreams += "<li><b>" + tr( "Strumień" ) + " " + QString::number( subtitlesStreamCount ) + "</b></li>";
	if ( currentPlaying )
		subtitlesStreams += "</u>";
	else
		subtitlesStreams += "</a>";
	subtitlesStreams += "<ul>";
	if ( !title.isEmpty() )
		subtitlesStreams += "<li><b>" + tr( "tytuł" ) + ":</b> " + title + "</li>";
	if ( streamName == "fileSubs" )
		subtitlesStreams += "<li><b>" + tr( "załadowane z pliku" ) + "</b></li>";
	if ( !codecName.isEmpty() )
		subtitlesStreams += "<li><b>" + tr( "format" ) + ":</b> " + codecName + "</li>";
	printOtherInfo( other_info, subtitlesStreams );
	subtitlesStreams += "</ul></ul>";
}
void DemuxerThr::emitInfo()
{
	QString info;
	if ( url.left( 7 ) == "file://" )
	{
		const QString pth = url.right( url.length() - 7 );
		info += "<b>" + tr( "Ścieżka do pliku" ) + ": </b> " + filePath( pth ) + "<br/>";
		info += "<b>" + tr( "Nazwa pliku" ) + ": </b> " + fileName( pth ) + "<br/>";
	}
	else
		info = "<b>" + tr( "Adres" ) + ":</b> " + url + "<br>";
	if ( demuxer->bitrate() > 0 )
		info += "<b>" + tr( "Bitrate" ) + ":</b> " + QString::number( demuxer->bitrate() ) + "kbps<br/>";
	info += "<b>" + tr( "Format" ) + ":</b> " + demuxer->name();

	if ( !demuxer->image().isNull() )
		info += "<br/><br/><a href='save_cover'>" + tr( "Zapisz okładkę" ) + "</a>";

	const QList< QMPlay2Tag > tags = demuxer->tags();
	if ( !tags.isEmpty() )
		info += "<br/>";
	QString formatTitle = demuxer->title();
	if ( formatTitle.isEmpty() )
	{
		if ( !name.isEmpty() )
			info += "<br/><b>" + tr( "Tytuł" ) + ":</b> " + name;
		formatTitle = name;
	}
	foreach ( QMPlay2Tag tag, demuxer->tags() )
		if ( !tag.first.isEmpty() )
			info += "<br/><b>" + StreamInfo::getTagName( tag.first ) + ":</b> " + tag.second;

	bool once = false;
	int chapterCount = 0;
	foreach ( Demuxer::ChapterInfo chapter, demuxer->getChapters() )
	{
		if ( !once )
		{
			info += "<p style='margin-bottom: 0px;'><b><big>" + tr( "Rozdziały" ) + ":</big></b></p>";
			once = true;
		}
		info += "<ul style='margin-top: 0px; margin-bottom: 0px;'>";
		info += "<li><a href='seek:" + QString::number( chapter.start ) + "'>" + ( chapter.title.isEmpty() ? tr( "Rozdział" ) + " " + QString::number( ++chapterCount ) : chapter.title ) + "</a></li>";
		info += "</ul>";
	}

	bool videoPlaying = false, audioPlaying = false;

	const QList< StreamInfo * > streamsInfo = demuxer->streamsInfo();
	QString videoStreams, audioStreams, subtitlesStreams, attachmentStreams;
	int videoStreamCount = 0, audioStreamCount = 0, subtitlesStreamCount = 0, i = 0;
	foreach ( StreamInfo *streamInfo, streamsInfo )
	{
		switch ( streamInfo->type )
		{
			case QMPLAY2_TYPE_VIDEO:
			{
				const bool currentPlaying = getCurrentPlaying( playC.videoStream, streamsInfo, streamInfo );
				videoStreams += "<ul style='margin-top: 0px; margin-bottom: 0px;'><li>";
				if ( currentPlaying )
				{
					videoPlaying = true;
					videoStreams += "<u>";
				}
				else
					videoStreams += "<a style='text-decoration: none; color: black;' href='stream:video" + QString::number( i ) + "'>";
				videoStreams += "<b>" + tr( "Strumień" ) + " " + QString::number( ++videoStreamCount ) + "</b>";
				if ( currentPlaying )
					videoStreams += "</u>" + getWriterName( ( AVThread * )playC.vThr );
				else
					videoStreams += "</a>";
				videoStreams += "</li><ul>";
				if ( !streamInfo->title.isEmpty() )
					videoStreams += "<li><b>" + tr( "tytuł" ) + ":</b> " + streamInfo->title + "</li>";
				if ( !streamInfo->codec_name.isEmpty() )
					videoStreams += "<li><b>" + tr( "kodek" ) + ":</b> " + streamInfo->codec_name + "</li>";
				videoStreams += "<li><b>" + tr( "wielkość" ) + ":</b> " + QString::number( streamInfo->W ) + "x" + QString::number( streamInfo->H ) + "</li>";
				videoStreams += "<li><b>" + tr( "proporcje" ) + ":</b> " + QString::number( streamInfo->aspect_ratio ) + "</li>";
				if ( streamInfo->FPS )
					videoStreams += "<li><b>" + tr( "FPS" ) + ":</b> " + QString::number( streamInfo->FPS ) + "</li>";
				if ( streamInfo->bitrate )
					videoStreams += "<li><b>" + tr( "bitrate" ) + ":</b> " + QString::number( streamInfo->bitrate / 1000 ) + "kbps</li>";
				printOtherInfo( streamInfo->other_info, videoStreams );
				videoStreams += "</ul></ul>";
			} break;
			case QMPLAY2_TYPE_AUDIO:
			{
				const bool currentPlaying = getCurrentPlaying( playC.audioStream, streamsInfo, streamInfo );
				audioStreams += "<ul style='margin-top: 0px; margin-bottom: 0px;'><li>";
				if ( currentPlaying )
				{
					audioPlaying = true;
					audioStreams += "<u>";
				}
				else
					audioStreams += "<a style='text-decoration: none; color: black;' href='stream:audio" + QString::number( i ) + "'>";
				audioStreams += "<b>" + tr( "Strumień" ) + " " + QString::number( ++audioStreamCount ) + "</b>";
				if ( currentPlaying )
					audioStreams += "</u>" + getWriterName( ( AVThread * )playC.aThr );
				else
					audioStreams += "</a>";
				audioStreams += "</li><ul>";
				if ( !streamInfo->title.isEmpty() )
					audioStreams += "<li><b>" + tr( "tytuł" ) + ":</b> " + streamInfo->title + "</li>";
				if ( !streamInfo->artist.isEmpty() )
					audioStreams += "<li><b>" + tr( "artysta" ) + ":</b> " + streamInfo->artist + "</li>";
				if ( !streamInfo->codec_name.isEmpty() )
					audioStreams += "<li><b>" + tr( "kodek" ) + ":</b> " + streamInfo->codec_name + "</li>";
				audioStreams += "<li><b>" + tr( "próbkowanie" ) + ":</b> " + QString::number( streamInfo->sample_rate ) + "Hz</li>";

				QString channels;
				if ( streamInfo->channels == 1 )
					channels = tr( "mono" );
				else if ( streamInfo->channels == 2 )
					channels = tr( "stereo" );
				else
					channels = QString::number( streamInfo->channels );
				audioStreams += "<li><b>" + tr( "kanały" ) + ":</b> " + channels + "</li>";

				if ( streamInfo->bitrate )
					audioStreams += "<li><b>" + tr( "bitrate" ) + ":</b> " + QString::number( streamInfo->bitrate / 1000 ) + "kbps</li>";
				printOtherInfo( streamInfo->other_info, audioStreams );
				audioStreams += "</ul></ul>";
			} break;
			case QMPLAY2_TYPE_SUBTITLE:
				addSubtitleStream( getCurrentPlaying( playC.subtitlesStream, streamsInfo, streamInfo ), subtitlesStreams, i, ++subtitlesStreamCount, "subtitles", streamInfo->codec_name, streamInfo->title, streamInfo->other_info );
				break;
			case QMPLAY2_TYPE_ATTACHMENT:
			{
				attachmentStreams += "<ul style='margin-top: 0px; margin-bottom: 0px;'>";
				attachmentStreams += "<li><b>" + streamInfo->title + "</b> - " + sizeString( streamInfo->data.size() ) + "</li>";
				attachmentStreams += "</ul>";
			} break;
			default:
				break;
		}
		++i;
	}
	i = 0;
	foreach ( QString fName, playC.fileSubsList )
		addSubtitleStream( fName == playC.fileSubs, subtitlesStreams, i++, ++subtitlesStreamCount, "fileSubs", QString(), fileName( fName ) );

	if ( !videoStreams.isEmpty() )
		info += "<p style='margin-bottom: 0px;'><b><big>" + tr( "Strumienie obrazu" ) + ":</big></b></p>" + videoStreams;
	if ( !audioStreams.isEmpty() )
		info += "<p style='margin-bottom: 0px;'><b><big>" + tr( "Strumienie dźwięku" ) + ":</big></b></p>" + audioStreams;
	if ( !subtitlesStreams.isEmpty() )
		info += "<p style='margin-bottom: 0px;'><b><big>" + tr( "Strumienie napisów" ) + ":</big></b></p>" + subtitlesStreams;
	if ( !attachmentStreams.isEmpty() )
		info += "<p style='margin-bottom: 0px;'><b><big>" + tr ( "Dołączone pliki" ) + ":</big></b></p>" + attachmentStreams;

	emit playC.setInfo( info, videoPlaying, audioPlaying );
	emit playC.updateCurrentEntry( formatTitle, demuxer->length() );
	emit playC.updateWindowTitle( formatTitle.isEmpty() ? fileName( url, false ) : formatTitle );
}

bool DemuxerThr::mustReloadStreams()
{
	if
	(
		playC.reload ||
		( playC.choosenAudioStream     > -1 && playC.choosenAudioStream     != playC.audioStream     ) ||
		( playC.choosenVideoStream     > -1 && playC.choosenVideoStream     != playC.videoStream     ) ||
		( playC.choosenSubtitlesStream > -1 && playC.choosenSubtitlesStream != playC.subtitlesStream )
	)
	{
		if ( playC.frame_last_delay <= 0.0 && playC.videoStream > -1 )
			playC.frame_last_delay = 1.0 / demuxer->streamsInfo()[ playC.videoStream ]->FPS;
		playC.reload = true;
		return true;
	}
	return false;
}
bool DemuxerThr::bufferedPackets( int vS, int aS, int p )
{
	return
	(
		( playC.vThr && vS >= p && playC.aThr && aS >= p ) ||
		( !playC.vThr && playC.aThr && aS >= p )           ||
		( playC.vThr && !playC.aThr && vS >= p )
	);
}
bool DemuxerThr::emptyBuffers( int vS, int aS )
{
	return ( playC.vThr && playC.aThr && ( !vS || !aS ) ) || ( !playC.vThr && playC.aThr && !aS ) || ( playC.vThr && !playC.aThr && !vS );
}
bool DemuxerThr::canBreak( const AVThread *avThr1, const AVThread *avThr2 )
{
	return ( !avThr1 || avThr1->isWaiting() ) && ( !avThr2 || avThr2->isWaiting() );
}
void DemuxerThr::getAVBuffersSize( int &vS, int &aS, qint64 *buffered, double *bufferedTime )
{
	double aTime = 0.0, vTime = 0.0;

	if ( buffered )
		*buffered = 0;

	playC.vPackets.lock();
	vS = playC.vPackets.packetCount();
	if ( buffered )
	{
		*buffered += playC.vPackets.size();
		vTime = playC.vPackets.duration();
	}
	playC.vPackets.unlock();

	playC.aPackets.lock();
	aS = playC.aPackets.packetCount();
	if ( buffered )
	{
		*buffered += playC.aPackets.size();
		aTime = playC.aPackets.duration();
	}
	playC.aPackets.unlock();

	if ( buffered && bufferedTime )
	{
		if ( vS && vTime )
			*bufferedTime = vTime;
		else
			*bufferedTime = aTime;
	}
}
void DemuxerThr::clearBuffers()
{
	playC.vPackets.clear();
	playC.aPackets.clear();
	playC.clearSubtitlesBuffer();
}

void DemuxerThr::stopVADecSlot()
{
	clearBuffers();

	playC.stopVDec();
	playC.stopADec();

	stopVAMutex.unlock();

	endMutex.lock(); //Czeka do czasu zniszczenia demuxer'a, jeżeli wcześniej mutex był zablokowany (wykonał się z wątku)
	endMutex.unlock(); //odblokowywuje mutex
}
void DemuxerThr::updateCover( const QString &title, const QString &artist, const QString &album, const QByteArray &cover )
{
	const QImage coverImg = QImage::fromData( cover );
	if ( !coverImg.isNull() )
	{
		if ( this->title == title && this->artist == artist && this->album == album )
			emit playC.updateImage( coverImg );
		QDir dir( QMPlay2Core.getSettingsDir() );
		dir.mkdir( "Covers" );
		QFile f( getCoverFile( title, artist, album ) );
		if ( f.open( QFile::WriteOnly ) )
		{
			f.write( cover );
			f.close();
			emit QMPlay2Core.coverFile( f.fileName() );
		}
	}
}