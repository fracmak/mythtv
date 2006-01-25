#include <qapplication.h>
#include <qsqldatabase.h>
#include <qdatetime.h>
#include <qfile.h>
#include <qdir.h>
#include <qurl.h>
#include <qthread.h>
#include <qwaitcondition.h>
#include <qregexp.h>

#include <cstdlib>
#include <cerrno>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include "../../config.h"
#ifdef HAVE_SYS_SOUNDCARD_H
    #include <sys/soundcard.h>
#elif HAVE_SOUNDCARD_H
    #include <soundcard.h>
#endif
#include <sys/ioctl.h>

#include <list>
#include <iostream>
using namespace std;

#include <sys/stat.h>
#ifdef linux
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

#include "libmyth/exitcodes.h"
#include "libmyth/mythcontext.h"
#include "libmyth/util.h"
#include "libmyth/mythdbcon.h"

#include "mainserver.h"
#include "scheduler.h"
#include "httpstatus.h"
#include "programinfo.h"
#include "jobqueue.h"
#include "autoexpire.h"
#include "previewgenerator.h"

/** Milliseconds to wait for an existing thread from
 *  process request thread pool.
 */
#define PRT_TIMEOUT 10
/** Number of threads in process request thread pool at startup. */
#define PRT_STARTUP_THREAD_COUNT 5

class ProcessRequestThread : public QThread
{
  public:
    ProcessRequestThread(MainServer *ms) { parent = ms; }
   
    void setup(RefSocket *sock)
    {
        lock.lock();
        socket = sock;
        socket->UpRef();
        lock.unlock();
        waitCond.wakeOne();
    }

    void killit(void)
    {
        lock.lock();
        threadlives = false;
        lock.unlock();
        waitCond.wakeOne();
    }

    virtual void run()
    {
        threadlives = true;

        lock.lock();

        while (1)
        {
            waitCond.wait(&lock);

            if (!threadlives)
                break;

            parent->ProcessRequest(socket);
            parent->MarkUnused(this);
            socket->DownRef();
            socket = NULL;
        }

        lock.unlock();
    }

  private:
    MainServer *parent;

    RefSocket *socket;

    QMutex lock;
    QWaitCondition waitCond;
    bool threadlives;
};

MainServer::MainServer(bool master, int port, int statusport, 
                       QMap<int, EncoderLink *> *tvList,
                       Scheduler *sched, AutoExpire *expirer)
{
    m_sched = sched;
    m_expirer = expirer;

    ismaster = master;
    masterServer = NULL;

    encoderList = tvList;
    AutoExpire::Update(encoderList, true);

    for (int i = 0; i < PRT_STARTUP_THREAD_COUNT; i++)
    {
        ProcessRequestThread *prt = new ProcessRequestThread(this);
        prt->start();
        threadPool.push_back(prt);
    }

    recordfileprefix = gContext->GetFilePrefix();

    masterBackendOverride = gContext->GetSetting("MasterBackendOverride", 0);

    mythserver = new MythServer(port);
    if (!mythserver->ok())
    {
        VERBOSE(VB_IMPORTANT, QString("Failed to bind port %1. Exiting.")
                .arg(port));
        exit(BACKEND_BUGGY_EXIT_NO_BIND_MAIN);
    }

    connect(mythserver, SIGNAL(newConnect(RefSocket *)), 
            SLOT(newConnection(RefSocket *)));
    connect(mythserver, SIGNAL(endConnect(RefSocket *)), 
            SLOT(endConnection(RefSocket *)));

    statusserver = new HttpStatus(this, statusport);    
    if (!statusserver->ok())
    { 
        VERBOSE(VB_IMPORTANT, QString("Failed to bind to port %1. Exiting.")
                .arg(statusport));
        exit(BACKEND_BUGGY_EXIT_NO_BIND_STATUS);
    }

    gContext->addListener(this);

    if (!ismaster)
    {
        masterServerReconnect = new QTimer(this);
        connect(masterServerReconnect, SIGNAL(timeout()), this, 
                SLOT(reconnectTimeout()));
        masterServerReconnect->start(1000, true);
    }

    deferredDeleteTimer = new QTimer(this);
    connect(deferredDeleteTimer, SIGNAL(timeout()), this,
            SLOT(deferredDeleteSlot()));
    deferredDeleteTimer->start(30 * 1000);

    if (sched)
        sched->SetMainServer(this);
}

MainServer::~MainServer()
{
    delete mythserver;

    if (statusserver)
        delete statusserver;
    if (masterServerReconnect)
        delete masterServerReconnect;
    if (deferredDeleteTimer)
        delete deferredDeleteTimer;
}

void MainServer::newConnection(RefSocket *socket)
{
    connect(socket, SIGNAL(readyRead()), this, SLOT(readSocket()));
}

void MainServer::readSocket(void)
{
    RefSocket *socket = (RefSocket *)sender();

    PlaybackSock *testsock = getPlaybackBySock(socket);
    if (testsock && testsock->isExpectingReply())
        return;

    readReadyLock.lock();

    //if (socket->IsInProcess())
    //{
    //    VERBOSE(VB_IMPORTANT, "Overlapping calls to readSocket.");
    //    readReadyLock.unlock();
    //    return;
    //}

    // will be set to false after processed by worker thread.
    //socket->SetInProcess(true);

    MythTimer t;
    t.start();
    ProcessRequestThread *prt = NULL;
    while (!prt)
    {
        threadPoolLock.lock();
        if (!threadPool.empty())
        {
            prt = threadPool.back();
            threadPool.pop_back();
        }
        threadPoolLock.unlock();

        if (t.elapsed() > PRT_TIMEOUT)
            break;

        if (!prt)
        {
            VERBOSE(VB_IMPORTANT, "Waiting for a process request thread..");
            usleep(1000); /* 1 millisecond */
        }
    }

    if (!prt)
    {
        threadPoolLock.lock();
        VERBOSE(VB_IMPORTANT, "Adding a new process request thread");
        prt = new ProcessRequestThread(this);
        prt->start();
        usleep(50000); /* Wait 50 milliseconds for start to actually run. */
        threadPoolLock.unlock();
    }

    prt->setup(socket);

    readReadyLock.unlock();
}

void MainServer::ProcessRequest(RefSocket *sock)
{
    sock->Lock();

    //while (sock->bytesAvailable() > 0)
    if (sock->bytesAvailable() > 0)
    {
        ProcessRequestWork(sock);
    }

    sock->Unlock();
    //sock->SetInProcess(false);
}

void MainServer::ProcessRequestWork(RefSocket *sock)
{
    QStringList listline;
    if (!ReadStringList(sock, listline))
        return;

    QString line = listline[0];

    line = line.simplifyWhiteSpace();
    QStringList tokens = QStringList::split(" ", line);
    QString command = tokens[0];
    //cerr << "command='" << command << "'\n";
    if (command == "MYTH_PROTO_VERSION")
    {
        if (tokens.size() < 2)
            VERBOSE(VB_IMPORTANT, "Bad MYTH_PROTO_VERSION command");
        else
            HandleVersion(sock,tokens[1]);
        return;
    }
    else if (command == "ANN")
    {
        if (tokens.size() < 3 || tokens.size() > 4)
            VERBOSE(VB_IMPORTANT, "Bad ANN query");
        else
            HandleAnnounce(listline, tokens, sock);
        return;
    }
    else if (command == "DONE")
    {
        HandleDone(sock);
        return;
    }

    PlaybackSock *pbs = getPlaybackBySock(sock);
    if (!pbs)
    {
        VERBOSE(VB_IMPORTANT, "unknown socket");
        return;
    }

    // Increase refcount while using..
    pbs->UpRef();

    if (command == "QUERY_RECORDINGS")
    {
        if (tokens.size() != 2)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_RECORDINGS query");
        else
            HandleQueryRecordings(tokens[1], pbs);
    }
    else if (command == "QUERY_FREE_SPACE")
    {
        HandleQueryFreeSpace(pbs, false);
    }
    else if (command == "QUERY_FREE_SPACE_LIST")
    {
        HandleQueryFreeSpace(pbs, true);
    }
    else if (command == "QUERY_LOAD")
    {
        HandleQueryLoad(pbs);
    }
    else if (command == "QUERY_UPTIME")
    {
        HandleQueryUptime(pbs);
    }
    else if (command == "QUERY_MEMSTATS")
    {
        HandleQueryMemStats(pbs);
    }
    else if (command == "QUERY_CHECKFILE")
    {
        HandleQueryCheckFile(listline, pbs);
    }
    else if (command == "QUERY_GUIDEDATATHROUGH")
    {
        HandleQueryGuideDataThrough(pbs);
    }
    else if (command == "STOP_RECORDING")
    {
        HandleStopRecording(listline, pbs);
    }
    else if (command == "CHECK_RECORDING")
    {
        HandleCheckRecordingActive(listline, pbs);
    }
    else if (command == "DELETE_RECORDING")
    {
        HandleDeleteRecording(listline, pbs, false);
    }
    else if (command == "FORCE_DELETE_RECORDING")
    {
        HandleDeleteRecording(listline, pbs, true);
    }
    else if (command == "RESCHEDULE_RECORDINGS")
    {
        if (tokens.size() != 2)
            VERBOSE(VB_IMPORTANT, "Bad RESCHEDULE_RECORDINGS request");
        else
            HandleRescheduleRecordings(tokens[1].toInt(), pbs);
    }
    else if (command == "FORGET_RECORDING")
    {
        HandleForgetRecording(listline, pbs);
    }
    else if (command == "QUERY_GETALLPENDING")
    {
        if (tokens.size() == 1)
            HandleGetPendingRecordings(pbs);
        else if (tokens.size() == 2)
            HandleGetPendingRecordings(pbs, tokens[1]);
        else
            HandleGetPendingRecordings(pbs, tokens[1], tokens[2].toInt());
    }
    else if (command == "QUERY_GETALLSCHEDULED")
    {
        HandleGetScheduledRecordings(pbs);
    }
    else if (command == "QUERY_GETCONFLICTING")
    {
        HandleGetConflictingRecordings(listline, pbs);
    }
    else if (command == "QUERY_GETEXPIRING")
    {
        HandleGetExpiringRecordings(pbs);
    }
    else if (command == "GET_FREE_RECORDER")
    {
        HandleGetFreeRecorder(pbs);
    }
    else if (command == "GET_FREE_RECORDER_COUNT")
    {
        HandleGetFreeRecorderCount(pbs);
    }
    else if (command == "GET_FREE_RECORDER_LIST")
    {
        HandleGetFreeRecorderList(pbs);
    }
    else if (command == "GET_NEXT_FREE_RECORDER")
    {
        HandleGetNextFreeRecorder(listline, pbs);
    }
    else if (command == "QUERY_RECORDER")
    {
        if (tokens.size() != 2)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_RECORDER");
        else
            HandleRecorderQuery(listline, tokens, pbs);
    }
    else if (command == "QUERY_REMOTEENCODER")
    {
        if (tokens.size() != 2)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_REMOTEENCODER");
        else
            HandleRemoteEncoder(listline, tokens, pbs);
    }
    else if (command == "GET_RECORDER_FROM_NUM")
    {
        HandleGetRecorderFromNum(listline, pbs);
    }
    else if (command == "GET_RECORDER_NUM")
    {
        HandleGetRecorderNum(listline, pbs);
    }
    else if (command == "QUERY_FILETRANSFER")
    {
        if (tokens.size() != 2)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_FILETRANSFER");
        else
            HandleFileTransferQuery(listline, tokens, pbs);
    }
    else if (command == "QUERY_GENPIXMAP")
    {
        HandleGenPreviewPixmap(listline, pbs);
    }
    else if (command == "QUERY_PIXMAP_LASTMODIFIED")
    {
        HandlePixmapLastModified(listline, pbs);
    }
    else if (command == "QUERY_ISRECORDING") 
    {
        HandleIsRecording(listline, pbs);
    }
    else if (command == "MESSAGE")
    {
        HandleMessage(listline, pbs);
    } 
    else if (command == "FILL_PROGRAM_INFO")
    {
        HandleFillProgramInfo(listline, pbs);
    }
    else if (command == "LOCK_TUNER")
    {
        HandleLockTuner(pbs);
    }
    else if (command == "FREE_TUNER")
    {
        if (tokens.size() != 2)
            VERBOSE(VB_IMPORTANT, "Bad FREE_TUNER query");
        else
            HandleFreeTuner(tokens[1].toInt(), pbs);
    }
    else if (command == "QUERY_IS_ACTIVE_BACKEND")
    {
        if (tokens.size() != 1)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_IS_ACTIVE_BACKEND");
        else
            HandleIsActiveBackendQuery(listline, pbs);
    }
    else if (command == "QUERY_COMMBREAK")
    {
        if (tokens.size() != 3)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_COMMBREAK");
        else
            HandleCommBreakQuery(tokens[1], tokens[2], pbs);
    }
    else if (command == "QUERY_CUTLIST")
    {
        if (tokens.size() != 3)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_CUTLIST");
        else
            HandleCutlistQuery(tokens[1], tokens[2], pbs);
    }
    else if (command == "QUERY_BOOKMARK")
    {
        if (tokens.size() != 3)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_BOOKMARK");
        else
            HandleBookmarkQuery(tokens[1], tokens[2], pbs);
    }
    else if (command == "SET_BOOKMARK")
    {
        if (tokens.size() != 5)
            VERBOSE(VB_IMPORTANT, "Bad SET_BOOKMARK");
        else
            HandleSetBookmark(tokens, pbs);
    }
    else if (command == "QUERY_SETTING")
    {
        if (tokens.size() != 3)
            VERBOSE(VB_IMPORTANT, "Bad QUERY_SETTING");
        else
            HandleSettingQuery(tokens, pbs);
    }
    else if (command == "SET_SETTING")
    {
        if (tokens.size() != 4)
            VERBOSE(VB_IMPORTANT, "Bad SET_SETTING");
        else
            HandleSetSetting(tokens, pbs);
    }
    else if (command == "ALLOW_SHUTDOWN")
    {
        if (tokens.size() != 1)
            VERBOSE(VB_IMPORTANT, "Bad ALLOW_SHUTDOWN");
        else
            HandleBlockShutdown(false, pbs);
    }
    else if (command == "BLOCK_SHUTDOWN")
    {
        if (tokens.size() != 1)
            VERBOSE(VB_IMPORTANT, "Bad BLOCK_SHUTDOWN");
        else
            HandleBlockShutdown(true, pbs);
    }
    else if (command == "SHUTDOWN_NOW")
    {
        if (tokens.size() != 1)
            VERBOSE(VB_IMPORTANT, "Bad SHUTDOWN_NOW query");
        else if (!ismaster)
        {
            QString halt_cmd = listline[1];
            if (!halt_cmd.isEmpty())
            {
                VERBOSE(VB_IMPORTANT, "Going down now as of Mainserver request!");
                system(halt_cmd.ascii());
            }
            else
                VERBOSE(VB_IMPORTANT,
                        "WARNING: Recieved an empty SHUTDOWN_NOW query!");
        }
    }
    else if (command == "BACKEND_MESSAGE")
    {
        QString message = listline[1];
        QStringList extra = listline[2];
        for (uint i = 3; i < listline.size(); i++)
            extra << listline[i];
        MythEvent me(message, extra);
        gContext->dispatch(me);
    }
    else if (command == "REFRESH_BACKEND")
    {
        VERBOSE(VB_IMPORTANT,"Reloading backend settings");
        HandleBackendRefresh(sock);
    }
    else if (command == "OK")
    {
        VERBOSE(VB_IMPORTANT, "Got 'OK' out of sequence.");
    }
    else if (command == "UNKNOWN_COMMAND")
    {
        VERBOSE(VB_IMPORTANT, "Got 'UNKNOWN_COMMAND' out of sequence.");
    }
    else
    {
        VERBOSE(VB_IMPORTANT, "Unknown command: " + command);

        QSocket *pbssock = pbs->getSocket();

        QStringList strlist;
        strlist << "UNKNOWN_COMMAND";
        
        SendResponse(pbssock, strlist);
    }

    // Decrease refcount..
    pbs->DownRef();
}

void MainServer::MarkUnused(ProcessRequestThread *prt)
{
    threadPoolLock.lock();
    threadPool.push_back(prt);
    threadPoolLock.unlock();
}

void MainServer::customEvent(QCustomEvent *e)
{
    QStringList broadcast;
    bool sendstuff = false;

    if ((MythEvent::Type)(e->type()) == MythEvent::MythEventMessage)
    {
        MythEvent *me = (MythEvent *)e;

        if (me->Message().left(11) == "AUTO_EXPIRE")
        {
            QStringList tokens = QStringList::split(" ", me->Message());
            if (tokens.size() != 3)
            {
                VERBOSE(VB_IMPORTANT, "Bad AUTO_EXPIRE message");
                return;
            }

            QDateTime startts = QDateTime::fromString(tokens[2], Qt::ISODate);
            ProgramInfo *pinfo = ProgramInfo::GetProgramFromRecorded(tokens[1],
                                                                     startts);
            if (pinfo)
            {
                // allow re-record if auto expired but not expired live buffers
                if (pinfo->recgroup != "LiveTV")
                    pinfo->ForgetHistory();
                DoHandleDeleteRecording(pinfo, NULL, false);
            }
            else
            {
                cerr << "Cannot find program info for '" << me->Message()
                     << "', while attempting to Auto-Expire." << endl;
            }

            return;
        }

        if (me->Message().left(21) == "RESCHEDULE_RECORDINGS" && m_sched)
        {
            QStringList tokens = QStringList::split(" ", me->Message());
            if (tokens.size() != 2)
            {
                VERBOSE(VB_IMPORTANT, "Bad RESCHEDULE_RECORDINGS message");
                return;
            }

            int recordid = tokens[1].toInt();
            m_sched->Reschedule(recordid);
            return;
        }

        if (me->Message().left(23) == "SCHEDULER_ADD_RECORDING" && m_sched)
        {
            ProgramInfo pi;
            QStringList list = me->ExtraDataList();
            if (!pi.FromStringList(list, 0))
            {
                VERBOSE(VB_IMPORTANT, "Bad SCHEDULER_ADD_RECORDING message");
                return;
            }

            m_sched->AddRecording(pi);
            return;
        }

        if (me->Message().left(23) == "UPDATE_RECORDING_STATUS" && m_sched)
        {
            QStringList tokens = QStringList::split(" ", me->Message());
            if (tokens.size() != 6)
            {
                VERBOSE(VB_IMPORTANT, "Bad UPDATE_RECORDING_STATUS message");
                return;
            }

            int cardid = tokens[1].toInt();
            QString chanid = tokens[2];
            QDateTime startts = QDateTime::fromString(tokens[3], Qt::ISODate);
            RecStatusType recstatus = RecStatusType(tokens[4].toInt());
            QDateTime recendts = QDateTime::fromString(tokens[5], Qt::ISODate);
            m_sched->UpdateRecStatus(cardid, chanid, startts, 
                                     recstatus, recendts);
            return;
        }

        if (me->Message().left(13) == "LIVETV_EXITED")
        {
            QString chainid = me->ExtraData();
            LiveTVChain *chain = GetExistingChain(chainid);
            if (chain)
                DeleteChain(chain);

            return;
        }

        if (me->Message() == "CLEAR_SETTINGS_CACHE")
            gContext->ClearSettingsCache();

        if (me->Message().left(6) == "LOCAL_")
            return;

        broadcast = "BACKEND_MESSAGE";
        broadcast << me->Message();
        broadcast += me->ExtraDataList();
        sendstuff = true;
    }

    if (sendstuff)
    {
        bool sendGlobal = false;
        if (ismaster && broadcast[1].left(7) == "GLOBAL_")
        {
            broadcast[1].replace(QRegExp("GLOBAL_"), "LOCAL_");
            MythEvent me(broadcast[1], broadcast[2]);
            gContext->dispatch(me);

            sendGlobal = true;
        }

        QPtrList<PlaybackSock> sentSet;

        // Make a local copy of the list, upping the refcount as we go..
        vector<PlaybackSock *> localPBSList;
        vector<PlaybackSock *>::iterator iter = playbackList.begin();
        for (; iter != playbackList.end(); iter++)
        {
            PlaybackSock *pbs = (*iter);
            pbs->UpRef();
            localPBSList.push_back(pbs);
        }

        for (iter = localPBSList.begin(); iter != localPBSList.end(); iter++)
        {
            PlaybackSock *pbs = (*iter);

            if (sentSet.containsRef(pbs) || pbs->IsDisconnected())
                continue;

            sentSet.append(pbs);

            RefSocket *sock = pbs->getSocket();
            sock->UpRef();

            qApp->unlock();
            sock->Lock();
            qApp->lock();

            if (broadcast[1] == "CLEAR_SETTINGS_CACHE")
            {
                if ((ismaster) &&
                    (pbs->isSlaveBackend() || pbs->wantsEvents()))
                    WriteStringList(sock, broadcast);
            }
            else if (sendGlobal)
            {
                if (pbs->isSlaveBackend())
                    WriteStringList(sock, broadcast);
            }
            else if (pbs->wantsEvents())
            {
                WriteStringList(sock, broadcast);
            }

            sock->Unlock();

            if (sock->DownRef())
            {
                // was deleted elsewhere, so the iterator's invalid.
                iter = playbackList.begin();
            }
        }

        // Done with the pbs list, so decrement all the instances..
        for (iter = localPBSList.begin(); iter != localPBSList.end(); iter++)
        {
            PlaybackSock *pbs = (*iter);
            pbs->DownRef();
        }
    }
}

void MainServer::HandleVersion(QSocket *socket,QString version)
{
    QStringList retlist;
    if (version != MYTH_PROTO_VERSION)
    {
        VERBOSE(VB_GENERAL,
                "MainServer::HandleVersion - Client speaks protocol version "
                + version + " but we speak " + MYTH_PROTO_VERSION + "!");
        retlist << "REJECT" << MYTH_PROTO_VERSION;
        WriteStringList(socket, retlist);
        HandleDone(socket);
        return;
    }

    retlist << "ACCEPT" << MYTH_PROTO_VERSION;
    WriteStringList(socket, retlist);
}

void MainServer::HandleAnnounce(QStringList &slist, QStringList commands, 
                                RefSocket *socket)
{
    QStringList retlist = "OK";

    vector<PlaybackSock *>::iterator iter = playbackList.begin();
    for (; iter != playbackList.end(); iter++)
    {
        PlaybackSock *pbs = (*iter);
        if (pbs->getSocket() == socket)
        {
            VERBOSE(VB_IMPORTANT, QString("Client %1 is trying to announce a socket "
                                    "multiple times.")
                                    .arg(commands[2]));
            WriteStringList(socket, retlist);
            return;
        }
    }

    if (commands[1] == "Playback" || commands[1] == "Monitor")
    {
        // Monitor connections are same as Playback but they don't
        // block shutdowns. See the Scheduler event loop for more.

        bool wantevents = commands[3].toInt();
        VERBOSE(VB_GENERAL, QString("MainServer::HandleAnnounce %1")
                                    .arg(commands[1]));
        VERBOSE(VB_IMPORTANT, QString("adding: %1 as a client (events: %2)")
                               .arg(commands[2]).arg(wantevents));
        PlaybackSock *pbs = new PlaybackSock(this, socket, commands[2], wantevents);
        pbs->setBlockShutdown(commands[1] == "Playback");
        playbackList.push_back(pbs);
    }
    else if (commands[1] == "SlaveBackend")
    {
        VERBOSE(VB_IMPORTANT, QString("adding: %1 as a slave backend server")
                               .arg(commands[2]));
        PlaybackSock *pbs = new PlaybackSock(this, socket, commands[2], false);
        pbs->setAsSlaveBackend();
        pbs->setIP(commands[3]);

        if (m_sched)
        {
            ProgramInfo pinfo;
            ProgramList slavelist;
            QStringList::Iterator sit = slist.at(1);
            while (sit != slist.end())
            {
                if (!pinfo.FromStringList(slist, sit))
                    break;
                slavelist.append(new ProgramInfo(pinfo));
            }
            m_sched->SlaveConnected(slavelist);
        }

        QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
        for (; iter != encoderList->end(); ++iter)
        {
            EncoderLink *elink = iter.data();
            if (elink->GetHostName() == commands[2])
                elink->SetSocket(pbs);
        }

        if (m_sched)
            m_sched->Reschedule(0);

        QString message = QString("LOCAL_SLAVE_BACKEND_ONLINE %2")
                                  .arg(commands[2]);
        MythEvent me(message);
        gContext->dispatch(me);

        pbs->setBlockShutdown(false);

        playbackList.push_back(pbs);

        AutoExpire::Update(encoderList, false);
    }
    else if (commands[1] == "FileTransfer")
    {
        VERBOSE(VB_GENERAL, "MainServer::HandleAnnounce FileTransfer");
        VERBOSE(VB_IMPORTANT, QString("adding: %1 as a remote file transfer")
                               .arg(commands[2]));
        QUrl qurl = slist[1];
        QString filename = LocalFilePath(qurl);

        FileTransfer *ft = new FileTransfer(filename, socket);

        fileTransferList.push_back(ft);

        retlist << QString::number(socket->socket());
        encodeLongLong(retlist, ft->GetFileSize());
    }

    WriteStringList(socket, retlist);
}

void MainServer::HandleDone(QSocket *socket)
{
    socket->close();
}

void MainServer::SendResponse(QSocket *socket, QStringList &commands)
{
    if (getPlaybackBySock(socket) || getFileTransferBySock(socket))
    {
        WriteStringList(socket, commands);
    }
    else
    {
        cerr << "Unable to write to client socket, as it's no longer there\n";
    }
}

void MainServer::HandleQueryRecordings(QString type, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();
    bool islocal = pbs->isLocal();
    QString playbackhost = pbs->getHostname();

    QString fs_db_name = "";

    QDateTime rectime = QDateTime::currentDateTime().addSecs(
                            -gContext->GetNumSetting("RecordOverTime"));
    RecIter ri;
    RecList schedList;
    if (m_sched)
        m_sched->getAllPending(&schedList);

    QString ip = gContext->GetSetting("BackendServerIP");
    QString port = gContext->GetSetting("BackendServerPort");
    QString chanorder = gContext->GetSetting("ChannelOrdering", "channum + 0");

    QMap<QString, int> inUseMap;
    QString inUseKey;
    QString inUseForWhat;
    QDateTime oneHourAgo = QDateTime::currentDateTime().addSecs(-61 * 60);

    MSqlQuery query(MSqlQuery::InitCon());

    query.prepare("SELECT DISTINCT chanid, starttime, recusage "
                  " FROM inuseprograms WHERE lastupdatetime >= :ONEHOURAGO ;");
    query.bindValue(":ONEHOURAGO", oneHourAgo);

    if (query.exec() && query.isActive() && query.size() > 0)
        while (query.next())
        {
            inUseKey = query.value(0).toString() + " " +
                       query.value(1).toDateTime().toString(Qt::ISODate);
            inUseForWhat = query.value(2).toString();

            if (!inUseMap.contains(inUseKey))
                inUseMap[inUseKey] = 0;

            if ((inUseForWhat == "player") ||
                (inUseForWhat == "preview player") ||
                (inUseForWhat == "PIP player"))
                inUseMap[inUseKey] = inUseMap[inUseKey] | FL_INUSEPLAYING;
            else if (inUseForWhat == "recorder")
                inUseMap[inUseKey] = inUseMap[inUseKey] | FL_INUSERECORDING;
        }


    QString thequery = "SELECT recorded.chanid,recorded.starttime,recorded.endtime,"
                       "recorded.title,recorded.subtitle,recorded.description,"
                       "recorded.hostname,channum,name,callsign,commflagged,cutlist,"
                       "recorded.autoexpire,editing,bookmark,recorded.category,"
                       "recorded.recgroup,record.dupin,record.dupmethod,"
                       "record.recordid,outputfilters,"
                       "recorded.seriesid,recorded.programid,recorded.filesize, "
                       "recorded.lastmodified, recorded.findid, "
                       "recorded.originalairdate, recorded.playgroup, "
                       "recorded.basename, recorded.progstart, "
                       "recorded.progend "
                       "FROM recorded "
                       "LEFT JOIN record ON recorded.recordid = record.recordid "
                       "LEFT JOIN channel ON recorded.chanid = channel.chanid "
                       "WHERE (recorded.deletepending = 0 OR "
                              "DATE_ADD(recorded.lastmodified, "
                                       "INTERVAL 5 MINUTE) <= NOW()) "
                       "ORDER BY recorded.starttime";

    if (type == "Delete")
        thequery += " DESC";

    thequery += ", " + chanorder + " DESC;";

    QStringList outputlist;
    QString fileprefix = gContext->GetFilePrefix();
    QMap<QString, QString> backendIpMap;
    QMap<QString, QString> backendPortMap;

    query.prepare(thequery);

    if (query.exec() && query.isActive() && query.size() > 0)
    {
        outputlist << QString::number(query.size());

        while (query.next())
        {
            ProgramInfo *proginfo = new ProgramInfo;

            proginfo->chanid = query.value(0).toString();
            proginfo->startts = query.value(29).toDateTime();
            proginfo->endts = query.value(30).toDateTime();
            proginfo->recstartts = query.value(1).toDateTime();
            proginfo->recendts = query.value(2).toDateTime();
            proginfo->title = QString::fromUtf8(query.value(3).toString());
            proginfo->subtitle = QString::fromUtf8(query.value(4).toString());
            proginfo->description = QString::fromUtf8(query.value(5).toString());
            proginfo->hostname = query.value(6).toString();

            proginfo->dupin = RecordingDupInType(query.value(17).toInt());
            proginfo->dupmethod = RecordingDupMethodType(query.value(18).toInt());
            proginfo->recordid = query.value(19).toInt();
            proginfo->chanOutputFilters = query.value(20).toString();
            proginfo->seriesid = query.value(21).toString();
            proginfo->programid = query.value(22).toString();
            proginfo->filesize = stringToLongLong(query.value(23).toString());
            proginfo->lastmodified =
                      QDateTime::fromString(query.value(24).toString(),
                                            Qt::ISODate);
            proginfo->findid = query.value(25).toInt();

            if (query.value(26).isNull())
            {
                proginfo->originalAirDate = proginfo->startts.date();
                proginfo->hasAirDate = false;
            }
            else
            {
                proginfo->originalAirDate =
                    QDate::fromString(query.value(26).toString(),Qt::ISODate);
                proginfo->hasAirDate = true;
            }

            QString basename = query.value(28).toString();

            if (proginfo->hostname.isEmpty() || proginfo->hostname.isNull())
                proginfo->hostname = gContext->GetHostName();

            if (!query.value(7).toString().isEmpty())
            {
                proginfo->chanstr = query.value(7).toString();
                proginfo->channame = QString::fromUtf8(query.value(8).toString());
                proginfo->chansign = QString::fromUtf8(query.value(9).toString());
            }
            else
            {
                proginfo->chanstr = "#" + proginfo->chanid;
                proginfo->channame = "#" + proginfo->chanid;
                proginfo->chansign = "#" + proginfo->chanid;
            }

            // Taken out of programinfo.cpp just to reduce the number of queries
            int flags = 0;
            flags |= (query.value(10).toInt() == 1) ? FL_COMMFLAG : 0;
            flags |= query.value(11).toString().length() > 1 ? FL_CUTLIST : 0;
            flags |= query.value(12).toInt() ? FL_AUTOEXP : 0;
            flags |= query.value(14).toString().length() > 1 ? FL_BOOKMARK : 0;

            inUseKey = query.value(0).toString() + " " +
                       query.value(1).toDateTime().toString(Qt::ISODate);
            if (inUseMap.contains(inUseKey))
                flags |= inUseMap[inUseKey];

            if (query.value(13).toInt())
            {
                flags |= FL_EDITING;
            }
            else if (query.value(10).toInt() == COMM_FLAG_PROCESSING)
            {
                if (JobQueue::IsJobRunning(JOB_COMMFLAG, proginfo))
                    flags |= FL_EDITING;
                else
                    proginfo->SetCommFlagged(COMM_FLAG_NOT_FLAGGED);
            }

            proginfo->programflags = flags;

            proginfo->category = QString::fromUtf8(query.value(15).toString());

            proginfo->recgroup = query.value(16).toString();
            proginfo->playgroup = query.value(27).toString();

            proginfo->recstatus = rsRecorded;
            if (proginfo->recendts > rectime)
            {
                for (ri = schedList.begin(); ri != schedList.end(); ri++)
                {
                    if ((*ri) && (*ri)->recstatus == rsRecording &&
                        proginfo->chanid == (*ri)->chanid &&
                        proginfo->recstartts == (*ri)->recstartts)
                    {
                        proginfo->recstatus = rsRecording;
                        break;
                    }
                }
            }

            QString lpath = fileprefix + "/" + basename;
            PlaybackSock *slave = NULL;
            QFile checkFile(lpath);

            if (proginfo->hostname != gContext->GetHostName())
                slave = getSlaveByHostname(proginfo->hostname);

            if ((masterBackendOverride && checkFile.exists()) || 
                (proginfo->hostname == gContext->GetHostName()) ||
                (!slave && checkFile.exists()))
            {
                if (islocal)
                    proginfo->pathname = lpath;
                else
                    proginfo->pathname = QString("myth://") + ip + ":" + port
                                         + "/" + basename;

                if (proginfo->filesize == 0)
                {
                    struct stat st;

                    long long size = 0;
                    if (stat(lpath.ascii(), &st) == 0)
                        size = st.st_size;

                    proginfo->filesize = size;

                    proginfo->SetFilesize(size);
                }
            }
            else
            {
                if (!slave)
                {
                    VERBOSE(VB_IMPORTANT,
                            "MainServer::HandleQueryRecordings()"
                            "\n\t\t\tCouldn't find backend for: " +
                            QString("\n\t\t\t%1 : \"%2\"")
                            .arg(proginfo->title).arg(proginfo->subtitle));

                    proginfo->filesize = 0;
                    proginfo->pathname = "file not found";
                }
                else
                {
                    if (proginfo->filesize == 0)
                    {
                        slave->FillProgramInfo(proginfo, playbackhost);

                        proginfo->SetFilesize(proginfo->filesize);
                    }
                    else
                    {
                        ProgramInfo *p = proginfo;
                        if (!backendIpMap.contains(p->hostname))
                            backendIpMap[p->hostname] =
                                gContext->GetSettingOnHost("BackendServerIp",
                                                           p->hostname);
                        if (!backendPortMap.contains(p->hostname))
                            backendPortMap[p->hostname] =
                                gContext->GetSettingOnHost("BackendServerPort",
                                                           p->hostname);
                        p->pathname = QString("myth://") +
                                      backendIpMap[p->hostname] + ":" +
                                      backendPortMap[p->hostname] + "/" +
                                      basename;
                    }
                }
            }

            if (slave)
                slave->DownRef();

            proginfo->ToStringList(outputlist);

            delete proginfo;
        }
    }
    else
        outputlist << "0";

    for (ri = schedList.begin(); ri != schedList.end(); ri++)
        delete (*ri);

    SendResponse(pbssock, outputlist);
}

void MainServer::HandleFillProgramInfo(QStringList &slist, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QString playbackhost = slist[1];

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 2);

    QString fileprefix = gContext->GetFilePrefix();
    QString lpath = pginfo->GetRecordFilename(fileprefix);
    QString ip = gContext->GetSetting("BackendServerIP");
    QString port = gContext->GetSetting("BackendServerPort");

    if (playbackhost == gContext->GetHostName())
        pginfo->pathname = lpath;
    else
        pginfo->pathname = QString("myth://") + ip + ":" + port
                           + "/" + pginfo->GetRecordBasename();

    struct stat st;

    long long size = 0;
    if (stat(lpath.ascii(), &st) == 0)
        size = st.st_size;

    pginfo->filesize = size;

    QStringList strlist;

    pginfo->ToStringList(strlist);

    delete pginfo;

    SendResponse(pbssock, strlist);
}

void *MainServer::SpawnDeleteThread(void *param)
{
    DeleteStruct *ds = (DeleteStruct *)param;

    MainServer *ms = ds->ms;
    ms->DoDeleteThread(ds);

    delete ds;

    return NULL;
}

void MainServer::DoDeleteThread(DeleteStruct *ds)
{
    // sleep a little to let frontends reload the recordings list
    // after deleteing a recording, then we can hammer the DB and filesystem
    sleep(3);
    usleep(rand()%2000);

    deletelock.lock();

    QString logInfo = QString("chanid %1 at %2")
                              .arg(ds->chanid).arg(ds->recstartts.toString());
                             
    QString name = QString("deleteThread%1%2").arg(getpid()).arg(rand());
    QFile checkFile(ds->filename);

    if (!MSqlQuery::testDBConnection())
    {
        QString msg = QString("ERROR opening database connection for Delete "
                              "Thread for chanid %1 recorded at %2.  Program "
                              "will NOT be deleted.")
                              .arg(ds->chanid).arg(ds->recstartts.toString());
        VERBOSE(VB_GENERAL, msg);
        gContext->LogEntry("mythbackend", LP_ERROR, "Delete Recording",
                           QString("Unable to open database connection for %1. "
                                   "Program will NOT be deleted.")
                                   .arg(logInfo));

        deletelock.unlock();
        return;
    }

    ProgramInfo *pginfo;
    pginfo = ProgramInfo::GetProgramFromRecorded(ds->chanid,
                                                 ds->recstartts);
    if (pginfo == NULL)
    {
        QString msg = QString("ERROR retrieving program info when trying to "
                              "delete program for chanid %1 recorded at %2. "
                              "Recording will NOT be deleted.")
                              .arg(ds->chanid).arg(ds->recstartts.toString());
        VERBOSE(VB_GENERAL, msg);
        gContext->LogEntry("mythbackend", LP_ERROR, "Delete Recording",
                           QString("Unable to retrieve program info for %1. "
                                   "Program will NOT be deleted.")
                                   .arg(logInfo));

        deletelock.unlock();
        return;
    }

    // allow deleting files where the recording failed (ie, filesize == 0)
    if ((!checkFile.exists()) &&
        (pginfo->filesize > 0) &&
        (!ds->forceMetadataDelete))
    {
        VERBOSE(VB_IMPORTANT, QString("ERROR when trying to delete file: %1. File "
                                "doesn't exist.  Database metadata"
                                "will not be removed.")
                                .arg(ds->filename));
        gContext->LogEntry("mythbackend", LP_WARNING, "Delete Recording",
                           QString("File %1 does not exist for %2 when trying "
                                   "to delete recording.")
                                   .arg(ds->filename).arg(logInfo));

        pginfo->SetDeleteFlag(false);
        delete pginfo;

        MythEvent me("RECORDING_LIST_CHANGE");
        gContext->dispatch(me);

        deletelock.unlock();
        return;
    }

    JobQueue::DeleteAllJobs(ds->chanid, ds->recstartts);

    LiveTVChain *tvchain = GetChainWithRecording(pginfo);
    if (tvchain)
        tvchain->DeleteProgram(pginfo);

    int err;
    QString filename = ds->filename;
    bool followLinks = gContext->GetNumSetting("DeletesFollowLinks", 0);

    VERBOSE(VB_FILE, QString("About to unlink/delete file: %1").arg(filename));
    if (followLinks)
    {
        QFileInfo finfo(filename);
        if (finfo.isSymLink() && (err = unlink(finfo.readLink().local8Bit())))
        {
            VERBOSE(VB_IMPORTANT, QString("Error deleting '%1' @ '%2', %3")
                    .arg(filename).arg(finfo.readLink().local8Bit())
                    .arg(strerror(errno)));
        }
    }
    if ((err = unlink(filename.local8Bit())))
        VERBOSE(VB_IMPORTANT, QString("Error deleting '%1', %2")
                .arg(filename).arg(strerror(errno)));
    
    sleep(2);

    if (checkFile.exists())
    {
        VERBOSE(VB_IMPORTANT,
            QString("Error deleting file: %1. Keeping metadata in database.")
                    .arg(ds->filename));
        gContext->LogEntry("mythbackend", LP_WARNING, "Delete Recording",
                           QString("File %1 for %2 could not be deleted.")
                                   .arg(ds->filename).arg(logInfo));

        pginfo->SetDeleteFlag(false);
        delete pginfo;

        MythEvent me("RECORDING_LIST_CHANGE");
        gContext->dispatch(me);

        deletelock.unlock();
        return;
    }

    filename = ds->filename + ".png";
    if (followLinks)
    {
        QFileInfo finfo(filename);
        if (finfo.isSymLink() && (err = unlink(finfo.readLink().local8Bit())))
        {
            VERBOSE(VB_IMPORTANT, QString("Error deleting '%1' @ '%2', %3")
                    .arg(filename).arg(finfo.readLink().local8Bit())
                    .arg(strerror(errno)));
        }
    }

    checkFile.setName(filename);
    if (checkFile.exists() && (err = unlink(filename.local8Bit())))
        VERBOSE(VB_IMPORTANT, QString("Error deleting '%1', %2")
                .arg(filename).arg(strerror(errno)));

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare("DELETE FROM recorded WHERE chanid = :CHANID AND "
                  "title = :TITLE AND starttime = :STARTTIME;");
    query.bindValue(":CHANID", ds->chanid);
    query.bindValue(":TITLE", ds->title.utf8());
    query.bindValue(":STARTTIME", ds->recstartts);

    if (!query.exec() || !query.isActive())
    {
        MythContext::DBError("Recorded program deletion", query);
        gContext->LogEntry("mythbackend", LP_ERROR, "Delete Recording",
                           QString("Error deleting recorded table for %1.")
                                   .arg(logInfo));
    }

    query.prepare("SELECT COUNT(*) FROM recorded "
                  "WHERE chanid = :CHANID AND progstart = :PROGSTART;");
    query.bindValue(":CHANID", ds->chanid);
    query.bindValue(":PROGSTART", pginfo->startts);
    if (!query.exec() || !query.isActive() || !query.next())
    {
        MythContext::DBError("Recorded program parts left query", query);
    }
    else if (query.value(0).toInt() == 0)
    {
        // No more recordings that are part of this Program so we can
        // cleanup these three tables
        query.prepare("DELETE FROM recordedrating "
                      "WHERE chanid = :CHANID AND starttime = :STARTTIME;");
        query.bindValue(":CHANID", ds->chanid);
        query.bindValue(":STARTTIME", pginfo->startts);
        if (!query.exec() || !query.isActive())
            MythContext::DBError("Recorded program delete recordedrating",
                                 query);

        query.prepare("DELETE FROM recordedprogram "
                      "WHERE chanid = :CHANID AND starttime = :STARTTIME;");
        query.bindValue(":CHANID", ds->chanid);
        query.bindValue(":STARTTIME", pginfo->startts);
        if (!query.exec() || !query.isActive())
            MythContext::DBError("Recorded program delete recordedprogram",
                                 query);

        query.prepare("DELETE FROM recordedcredits "
                      "WHERE chanid = :CHANID AND starttime = :STARTTIME;");
        query.bindValue(":CHANID", ds->chanid);
        query.bindValue(":STARTTIME", pginfo->startts);
        if (!query.exec() || !query.isActive())
            MythContext::DBError("Recorded program delete recordedcredits",
                                 query);
    }

    sleep(1);

    // Notify the frontend so it can requery for Free Space
    MythEvent me("RECORDING_LIST_CHANGE");
    gContext->dispatch(me);

    // sleep a little to let frontends reload the recordings list
    sleep(3);

    query.prepare("DELETE FROM recordedmarkup "
                  "WHERE chanid = :CHANID AND starttime = :STARTTIME;");
    query.bindValue(":CHANID", ds->chanid);
    query.bindValue(":STARTTIME", ds->recstartts);
    query.exec();

    if (!query.isActive())
    {
        MythContext::DBError("Recorded program delete recordedmarkup",
                             query);
        gContext->LogEntry("mythbackend", LP_ERROR, "Delete Recording",
                           QString("Error deleting recordedmarkup for %1.")
                                   .arg(logInfo));
    }

    if (pginfo->recgroup != "LiveTV")
        ScheduledRecording::signalChange(0);

    delete pginfo;

    deletelock.unlock();
}

void MainServer::HandleCheckRecordingActive(QStringList &slist, 
                                            PlaybackSock *pbs)
{
    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    int result = 0;

    if (ismaster && pginfo->hostname != gContext->GetHostName())
    {
        PlaybackSock *slave = getSlaveByHostname(pginfo->hostname);
        if (slave)
        {
            result = slave->CheckRecordingActive(pginfo);
            slave->DownRef();
        }
    }
    else
    {
        QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
        for (; iter != encoderList->end(); ++iter)
        {
            EncoderLink *elink = iter.data();

            if (elink->IsLocal() && elink->MatchesRecording(pginfo))
                result = iter.key();
        }
    }

    QStringList outputlist = QString::number(result);
    if (pbssock)
        SendResponse(pbssock, outputlist);

    delete pginfo;
    return;
}

void MainServer::HandleStopRecording(QStringList &slist, PlaybackSock *pbs)
{
    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    DoHandleStopRecording(pginfo, pbs);
}

void MainServer::DoHandleStopRecording(ProgramInfo *pginfo, PlaybackSock *pbs)
{
    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    if (ismaster && pginfo->hostname != gContext->GetHostName())
    {
        PlaybackSock *slave = getSlaveByHostname(pginfo->hostname);

        int num = -1;

        if (slave)
        {
            num = slave->StopRecording(pginfo);

            if (num > 0)
            {
                (*encoderList)[num]->StopRecording();
                pginfo->recstatus = rsRecorded;
                if (m_sched)
                    m_sched->UpdateRecStatus(pginfo);
            }
            if (pbssock)
            {
                QStringList outputlist = "0";
                SendResponse(pbssock, outputlist);
            }
 
            delete pginfo;
            slave->DownRef();
            return;
        }
        else
        {
            // If the slave is unreachable, we can assume that the 
            // recording has stopped and the status should be updated.
            // Continue so that the master can try to update the endtime
            // of the file is in a shared directory.
            pginfo->recstatus = rsRecorded;
            if (m_sched)
                m_sched->UpdateRecStatus(pginfo);
        }

    }

    int recnum = -1;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if (elink->IsLocal() && elink->MatchesRecording(pginfo))
        {
            recnum = iter.key();

            elink->StopRecording();

            while (elink->IsBusyRecording() ||
                   elink->GetState() == kState_ChangingState)
            {
                usleep(100);
            }

            if (ismaster)
            {
                pginfo->recstatus = rsRecorded;
                if (m_sched)
                    m_sched->UpdateRecStatus(pginfo);
            }
        }
    }

    if (pbssock)
    {
        QStringList outputlist = QString::number(recnum);
        SendResponse(pbssock, outputlist);
    }

    delete pginfo;
}

void MainServer::HandleDeleteRecording(QStringList &slist, PlaybackSock *pbs,
                                       bool forceMetadataDelete)
{
    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    DoHandleDeleteRecording(pginfo, pbs, forceMetadataDelete);
}

void MainServer::DoHandleDeleteRecording(ProgramInfo *pginfo, PlaybackSock *pbs,
                                         bool forceMetadataDelete)
{
    int resultCode = -1;
    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    QString fileprefix = gContext->GetFilePrefix();
    QString filename = pginfo->GetRecordFilename(fileprefix);

    // If this recording was made by a another recorder, and that
    // recorder is available, tell it to do the deletion.
    if (ismaster && pginfo->hostname != gContext->GetHostName())
    {
        PlaybackSock *slave = getSlaveByHostname(pginfo->hostname);

        int num = -1;

        if (slave) 
        {
            num = slave->DeleteRecording(pginfo, forceMetadataDelete);

            if (num > 0)
            {
                (*encoderList)[num]->StopRecording();
                pginfo->recstatus = rsRecorded;
                if (m_sched)
                    m_sched->UpdateRecStatus(pginfo);
            }

            if (pbssock)
            {
                QStringList outputlist = QString::number(num);
                SendResponse(pbssock, outputlist);
            }

            delete pginfo;
            slave->DownRef();
            return;
        }
    }

    // Tell all encoders to stop recordering to the file being deleted.
    // Hopefully this is never triggered.

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if (elink->IsLocal() && elink->MatchesRecording(pginfo))
        {
            resultCode = iter.key();

            elink->StopRecording();

            while (elink->IsBusyRecording() || 
                   elink->GetState() == kState_ChangingState)
            {
                usleep(100);
            }

            if (ismaster)
            {
                pginfo->recstatus = rsRecorded;
                if (m_sched)
                    m_sched->UpdateRecStatus(pginfo);
            }
        }
    }

    QFile checkFile(filename);
    bool fileExists = checkFile.exists();

    // Allow deleting of files where the recording failed meaning size == 0
    // But do not allow deleting of files that appear to be completely absent.
    // The latter condition indicates the filesystem containing the file is
    // most likely absent and deleting the file metadata is unsafe.
    if ((fileExists) || (pginfo->filesize == 0) || (forceMetadataDelete))
    {
        DeleteStruct *ds = new DeleteStruct;
        ds->ms = this;
        ds->filename = filename;
        ds->title = pginfo->title;
        ds->chanid = pginfo->chanid;
        ds->recstartts = pginfo->recstartts;
        ds->recendts = pginfo->recendts;
        ds->forceMetadataDelete = forceMetadataDelete;

        pginfo->SetDeleteFlag(true);

        pthread_t deleteThread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        pthread_create(&deleteThread, &attr, SpawnDeleteThread, ds);
    }
    else
    {
        QString logInfo = QString("chanid %1 at %2")
                              .arg(pginfo->chanid)
                              .arg(pginfo->recstartts.toString());
        VERBOSE(VB_IMPORTANT,
                QString("ERROR when trying to delete file: %1. File doesn't "
                        "exist.  Database metadata will not be removed.")
                        .arg(filename));
        gContext->LogEntry("mythbackend", LP_WARNING, "Delete Recording",
                           QString("File %1 does not exist for %2 when trying "
                                   "to delete recording.")
                                   .arg(filename).arg(logInfo));
        resultCode = -2;
    }

    if (pbssock)
    {
        QStringList outputlist = QString::number(resultCode);
        SendResponse(pbssock, outputlist);
    }

    // Tell MythTV frontends that the recording list needs to be updated.
    if ((fileExists) || (pginfo->filesize == 0) || (forceMetadataDelete))
    {
        MythEvent me("RECORDING_LIST_CHANGE");
        gContext->dispatch(me);
    }

    delete pginfo;
}

void MainServer::HandleRescheduleRecordings(int recordid, PlaybackSock *pbs)
{
    QStringList result;
    if (m_sched)
    {
        m_sched->Reschedule(recordid);
        result = QString::number(1);
    }
    else
        result = QString::number(0);

    if (pbs)
    {
        QSocket *pbssock = pbs->getSocket();
        if (pbssock)
            SendResponse(pbssock, result);
    }
}

void MainServer::HandleForgetRecording(QStringList &slist, PlaybackSock *pbs)
{
    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    pginfo->ForgetHistory();

    if (pbssock)
    {
        QStringList outputlist = QString::number(0);
        SendResponse(pbssock, outputlist);
    }

    delete pginfo;

}

void MainServer::HandleQueryFreeSpace(PlaybackSock *pbs, bool allHosts)
{    
    QStringList strlist;
    long long totalKB = -1, usedKB = -1;
    getDiskSpace(recordfileprefix, totalKB, usedKB);
    if (!allHosts)
    {
        encodeLongLong(strlist, totalKB);
        encodeLongLong(strlist, usedKB);
        SendResponse(pbs->getSocket(), strlist);
    }
    else
    {
        strlist<<gContext->GetHostName();
        encodeLongLong(strlist, totalKB);
        encodeLongLong(strlist, usedKB);

        QMap <QString, bool> backendsCounted;
        QString encoderHost;
        QMap<int, EncoderLink *>::Iterator eit = encoderList->begin();
        while (ismaster && eit != encoderList->end())
        {
            encoderHost = eit.data()->GetHostName();
            if (eit.data()->IsConnected() &&
                !eit.data()->IsLocal() &&
                !backendsCounted.contains(encoderHost))
            {
                backendsCounted[encoderHost] = true;

                eit.data()->GetFreeDiskSpace(totalKB, usedKB);
                strlist<<encoderHost;
                encodeLongLong(strlist, totalKB);
                encodeLongLong(strlist, usedKB);
            }
            ++eit;
        }
        SendResponse(pbs->getSocket(), strlist);
    }
}

void MainServer::HandleQueryLoad(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QStringList strlist;

    double loads[3];
    if (getloadavg(loads,3) == -1)
        strlist << "getloadavg() failed";
    else
        strlist << QString::number(loads[0])
                << QString::number(loads[1])
                << QString::number(loads[2]);

    SendResponse(pbssock, strlist);
}

void MainServer::HandleQueryUptime(PlaybackSock *pbs)
{
    QSocket    *pbssock = pbs->getSocket();
    QStringList strlist;
    time_t      uptime;

    if (getUptime(uptime))
        strlist << QString::number(uptime);
    else
        strlist << "Could not determine uptime.";

    SendResponse(pbssock, strlist);
}

void MainServer::HandleQueryMemStats(PlaybackSock *pbs)
{
    QSocket    *pbssock = pbs->getSocket();
    QStringList strlist;
    int         totalMB, freeMB, totalVM, freeVM;

    if (getMemStats(totalMB, freeMB, totalVM, freeVM))
        strlist << QString::number(totalMB) << QString::number(freeMB)
                << QString::number(totalVM) << QString::number(freeVM);
    else
        strlist << "Could not determine memory stats.";

    SendResponse(pbssock, strlist);
}

void MainServer::HandleQueryCheckFile(QStringList &slist, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    int exists = 0;

    if (ismaster && pginfo->hostname != gContext->GetHostName())
    {
        PlaybackSock *slave = getSlaveByHostname(pginfo->hostname);

        if (slave) 
        {
             exists = slave->CheckFile(pginfo);
             slave->DownRef();

             QStringList outputlist = QString::number(exists);
             SendResponse(pbssock, outputlist);
             delete pginfo;
             return;
        }
    }

    QUrl qurl(pginfo->pathname);
    QString cpath = LocalFilePath(qurl);
    QFile checkFile(cpath);

    if (checkFile.exists() == true)
        exists = 1;

    QStringList strlist = QString::number(exists);
    SendResponse(pbssock, strlist);

    delete pginfo;
}

void MainServer::getGuideDataThrough(QDateTime &GuideDataThrough)
{
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare("SELECT max(endtime) FROM program;");

    if (query.exec() && query.isActive() && query.size())
    {
        query.next();
        if (query.isValid())
            GuideDataThrough = QDateTime::fromString(query.value(0).toString(),
                                                     Qt::ISODate);
    }
}

void MainServer::HandleQueryGuideDataThrough(PlaybackSock *pbs)
{
    QDateTime GuideDataThrough;
    QSocket *pbssock = pbs->getSocket();
    QStringList strlist;

    getGuideDataThrough(GuideDataThrough);

    if (GuideDataThrough.isNull())
        strlist << QString("0000-00-00 00:00");
    else
        strlist << QDateTime(GuideDataThrough).toString("yyyy-MM-dd hh:mm");

    SendResponse(pbssock, strlist);
}

void MainServer::HandleGetPendingRecordings(PlaybackSock *pbs, QString table, int recordid)
{
    QSocket *pbssock = pbs->getSocket();

    QStringList strList;

    if (m_sched) {
        if (table == "") m_sched->getAllPending(strList);
        else {
            // We need a different connection from the scheduler proper
            // DDCon exists, although it's designed for other purposes.
            MSqlQueryInfo dbconn = MSqlQuery::DDCon();
            Scheduler *sched = new Scheduler(false, encoderList,
                                             table, &dbconn, m_sched);
            sched->FillRecordListFromDB(recordid);
            sched->getAllPending(strList);
            delete sched;

            MSqlQuery query(MSqlQuery::InitCon());
            query.prepare("SELECT NULL FROM record WHERE recordid = :RECID;");
            query.bindValue(":RECID", recordid);

            if (query.exec() && query.isActive() && query.size())
            {
                ScheduledRecording s;
                s.loadByID(recordid);
                if (s.getSearchType() == kManualSearch)
                    HandleRescheduleRecordings(recordid, NULL);
            }
            else if (recordid > 0)
            {
                query.prepare("DELETE FROM program WHERE manualid = :RECID;");
                query.bindValue(":RECID", recordid);
                query.exec();
            }
        }
    }
    else
    {
        strList << QString::number(0);
        strList << QString::number(0);
    }

    SendResponse(pbssock, strList);
}

void MainServer::HandleGetScheduledRecordings(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QStringList strList;

    if (m_sched)
        m_sched->getAllScheduled(strList);
    else
        strList << QString::number(0);

    SendResponse(pbssock, strList);
}

void MainServer::HandleGetConflictingRecordings(QStringList &slist,
                                                PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    QStringList strlist;

    if (m_sched)
        m_sched->getConflicting(pginfo, strlist);
    else
        strlist << QString::number(0);

    SendResponse(pbssock, strlist);

    delete pginfo;
}

void MainServer::HandleGetExpiringRecordings(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QStringList strList;

    if (m_expirer)
        m_expirer->GetAllExpiring(strList);
    else
        strList << QString::number(0);

    SendResponse(pbssock, strList);
}

void MainServer::HandleLockTuner(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();
    QString pbshost = pbs->getHostname();

    QStringList strlist;
    int retval;
    
    EncoderLink *encoder = NULL;
    QString enchost;
    
    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if (elink->IsLocal())
            enchost = gContext->GetHostName();
        else
            enchost = elink->GetHostName();

        if ((enchost == pbshost) &&
            (elink->IsConnected()) &&
            (!elink->IsBusy()) &&
            (!elink->IsTunerLocked()))
        {
            encoder = elink;
            break;
        }
    }
    
    if (encoder)
    {
        retval = encoder->LockTuner();

        if (retval != -1)
        {
            QString msg = QString("Cardid %1 LOCKed for external use on %2.")
                                  .arg(retval).arg(pbshost);
            VERBOSE(VB_GENERAL, msg);

            MSqlQuery query(MSqlQuery::InitCon());
            query.prepare("SELECT videodevice, audiodevice, "
                          "vbidevice "
                          "FROM capturecard "
                          "WHERE cardid = :CARDID ;");
            query.bindValue(":CARDID", retval);

            if (query.exec() && query.isActive() && query.size())
            {
                // Success
                query.next();
                strlist << QString::number(retval)
                        << query.value(0).toString()
                        << query.value(1).toString()
                        << query.value(2).toString();

                if (m_sched)
                    m_sched->Reschedule(0);

                SendResponse(pbssock, strlist);
                return;
            }
            else
            {
                cerr << "mainserver.o: Failed querying the db for a videodevice"
                     << endl;
            }
        }
        else
        {
            // Tuner already locked
            strlist << "-2" << "" << "" << "";
            SendResponse(pbssock, strlist);
            return;
        }
    }

    strlist << "-1" << "" << "" << "";
    SendResponse(pbssock, strlist);
}

void MainServer::HandleFreeTuner(int cardid, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();
    QStringList strlist;
    EncoderLink *encoder = NULL;
    
    QMap<int, EncoderLink *>::Iterator iter = encoderList->find(cardid);
    if (iter == encoderList->end())
    {
        VERBOSE(VB_IMPORTANT, QString("Unknown encoder: %1").arg(cardid));
        strlist << "FAILED";
    }
    else
    {
        encoder = iter.data();
        encoder->FreeTuner();
        
        QString msg = QString("Cardid %1 FREED from external use on %2.")
                              .arg(cardid).arg(pbs->getHostname());
        VERBOSE(VB_GENERAL, msg);

        if (m_sched)
            m_sched->Reschedule(0);
        
        strlist << "OK";
    }
    
    SendResponse(pbssock, strlist);
}

void MainServer::HandleGetFreeRecorder(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();
    QString pbshost = pbs->getHostname();

    QStringList strlist;
    int retval = -1;

    EncoderLink *encoder = NULL;
    QString enchost;

    bool lastcard = false;

    if (gContext->GetSetting("LastFreeCard", "0") == "1")
        lastcard = true;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if (!lastcard)
        {
            if (elink->IsLocal())
                enchost = gContext->GetHostName();
            else
                enchost = elink->GetHostName();

            if (enchost == pbshost && elink->IsConnected() &&
                !elink->IsBusy() && !elink->IsTunerLocked())
            {
                encoder = elink;
                retval = iter.key();
                VERBOSE(VB_RECORD, QString("Card %1 is local.")
                        .arg(iter.key()));
                break;
            }
        }

        if ((retval == -1 || lastcard) && elink->IsConnected() &&
            !elink->IsBusy() && !elink->IsTunerLocked())
        {
            encoder = elink;
            retval = iter.key();
        }
        VERBOSE(VB_RECORD, QString("Checking card %1. Best card so far %2")
                .arg(iter.key()).arg(retval));
    }

    strlist << QString::number(retval);
        
    if (encoder)
    {
        if (encoder->IsLocal())
        {
            strlist << gContext->GetSetting("BackendServerIP");
            strlist << gContext->GetSetting("BackendServerPort");
        }
        else
        {
            strlist << gContext->GetSettingOnHost("BackendServerIP", 
                                                  encoder->GetHostName(),
                                                  "nohostname");
            strlist << gContext->GetSettingOnHost("BackendServerPort", 
                                                  encoder->GetHostName(), "-1");
        }
    }
    else
    {
        strlist << "nohost";
        strlist << "-1";
    }

    SendResponse(pbssock, strlist);
}

void MainServer::HandleGetFreeRecorderCount(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QStringList strlist;
    int count = 0;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if ((elink->IsConnected()) &&
            (!elink->IsBusy()) &&
            (!elink->IsTunerLocked()))
        {
            count++;
        }
    }

    strlist << QString::number(count);
        
    SendResponse(pbssock, strlist);
}

void MainServer::HandleGetFreeRecorderList(PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QStringList strlist;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if ((elink->IsConnected()) &&
            (!elink->IsBusy()) &&
            (!elink->IsTunerLocked()))
        {
            strlist << QString::number(iter.key());
        }
    }

    if (strlist.size() == 0)
        strlist << "0";

    SendResponse(pbssock, strlist);
}

void MainServer::HandleGetNextFreeRecorder(QStringList &slist, 
                                           PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();
    QString pbshost = pbs->getHostname();

    QStringList strlist;
    int retval = -1;
    int currrec = slist[1].toInt();

    EncoderLink *encoder = NULL;
    QString enchost;

    VERBOSE(VB_RECORD, QString("Getting next free recorder after : %1")
            .arg(currrec));

    // find current recorder
    QMap<int, EncoderLink *>::Iterator iter, curr = encoderList->find(currrec);

    if (currrec > 0 && curr != encoderList->end())
    {
        // cycle through all recorders
        for (iter = curr;;)
        {
            EncoderLink *elink;

            // last item? go back
            if (++iter == encoderList->end())
            {
                iter = encoderList->begin();
            }

            elink = iter.data();

            if ((retval == -1) &&
                (elink->IsConnected()) &&
                (!elink->IsBusy()) &&
                (!elink->IsTunerLocked()))
            {
                encoder = elink;
                retval = iter.key();
            }

            // cycled right through? no more available recorders
            if (iter == curr) 
                break;
        }
    } 
    else 
    {
        HandleGetFreeRecorder(pbs);
        return;
    }


    strlist << QString::number(retval);
        
    if (encoder)
    {
        if (encoder->IsLocal())
        {
            strlist << gContext->GetSetting("BackendServerIP");
            strlist << gContext->GetSetting("BackendServerPort");
        }
        else
        {
            strlist << gContext->GetSettingOnHost("BackendServerIP", 
                                                  encoder->GetHostName(),
                                                  "nohostname");
            strlist << gContext->GetSettingOnHost("BackendServerPort", 
                                                  encoder->GetHostName(), "-1");
        }
    }
    else
    {
        strlist << "nohost";
        strlist << "-1";
    }

    SendResponse(pbssock, strlist);
}

void MainServer::HandleRecorderQuery(QStringList &slist, QStringList &commands,
                                     PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    int recnum = commands[1].toInt();

    QMap<int, EncoderLink *>::Iterator iter = encoderList->find(recnum);
    if (iter == encoderList->end())
    {
        VERBOSE(VB_IMPORTANT, QString("Unknown encoder: %1").arg(recnum));
        QStringList retlist = "bad";
        SendResponse(pbssock, retlist);
        return;
    }

    EncoderLink *enc = iter.data();  

    QString command = slist[1];

    QStringList retlist;

    if (command == "IS_RECORDING")
    {
        retlist << QString::number((int)enc->IsReallyRecording());
    }
    else if (command == "GET_FRAMERATE")
    {
        retlist << QString::number(enc->GetFramerate());
    }
    else if (command == "GET_FRAMES_WRITTEN")
    {
        long long value = enc->GetFramesWritten();
        encodeLongLong(retlist, value);
    }
    else if (command == "GET_FILE_POSITION")
    {
        long long value = enc->GetFilePosition();
        encodeLongLong(retlist, value);
    }
    else if (command == "GET_MAX_BITRATE")
    {
        long long value = enc->GetMaxBitrate();
        encodeLongLong(retlist, value);
    }
    else if (command == "GET_CURRENT_RECORDING")
    {
        ProgramInfo *info = enc->GetRecording();
        info->ToStringList(retlist);
        delete info;
    }
    else if (command == "GET_KEYFRAME_POS")
    {
        long long desired = decodeLongLong(slist, 2);

        long long value = enc->GetKeyframePosition(desired);
        encodeLongLong(retlist, value);
    }
    else if (command == "FILL_POSITION_MAP")
    {
        int start = slist[2].toInt();
        int end   = slist[3].toInt();

        for (int keynum = start; keynum <= end; keynum++)
        {
            long long value = enc->GetKeyframePosition(keynum);
            if (value != -1) 
            {
                encodeLongLong(retlist, keynum);
                encodeLongLong(retlist, value);
            }
        }

        if (!retlist.size())
            retlist << "ok";
    }
    else if (command == "GET_RECORDING")
    {
        ProgramInfo *pginfo = enc->GetRecording();
        if (pginfo)
        {
            pginfo->ToStringList(retlist);
            delete pginfo;
        }
        else
        {
            ProgramInfo dummy;
            dummy.ToStringList(retlist);
        }
    }
    else if (command == "FRONTEND_READY")
    {
        enc->FrontendReady();
        retlist << "ok";
    }
    else if (command == "CANCEL_NEXT_RECORDING")
    {
        QString cancel = slist[2];
        VERBOSE(VB_IMPORTANT, "Received: CANCEL_NEXT_RECORDING "<<cancel);
        enc->CancelNextRecording(cancel == "1");
        retlist << "ok";
    }
    else if (command == "SPAWN_LIVETV")
    {
        QString chainid = slist[2];
        LiveTVChain *chain = GetExistingChain(chainid);
        if (!chain)
        {
            chain = new LiveTVChain();
            chain->LoadFromExistingChain(chainid);
            AddToChains(chain);
        }

        chain->SetHostSocket(pbssock);

        enc->SpawnLiveTV(chain, slist[3].toInt());
        retlist << "ok";
    }
    else if (command == "STOP_LIVETV")
    {
        QString chainid = enc->GetChainID();
        enc->StopLiveTV();

        LiveTVChain *chain = GetExistingChain(chainid);
        if (chain)
        {
            chain->DelHostSocket(pbssock);
            if (chain->HostSocketCount() == 0)
            {
                DeleteChain(chain);
            }
        }

        retlist << "ok";
    }
    else if (command == "PAUSE")
    {
        enc->PauseRecorder();
        retlist << "ok";
    }
    else if (command == "FINISH_RECORDING")
    {
        enc->FinishRecording();
        retlist << "ok";
    }
    else if (command == "SET_LIVE_RECORDING")
    {
        int recording = slist[2].toInt();
        enc->SetLiveRecording(recording);
        retlist << "ok";
    }
    else if (command == "TOGGLE_INPUTS")
    {
        enc->ToggleInputs();
        retlist << "ok";
    }
    else if (command == "TOGGLE_CHANNEL_FAVORITE")
    {
        enc->ToggleChannelFavorite();
        retlist << "ok";
    }
    else if (command == "CHANGE_CHANNEL")
    {
        int direction = slist[2].toInt(); 
        enc->ChangeChannel(direction);
        retlist << "ok";
    }
    else if (command == "SET_CHANNEL")
    {
        QString name = slist[2];
        enc->SetChannel(name);
        retlist << "ok";
    }
    else if (command == "SET_SIGNAL_MONITORING_RATE")
    {
        int rate = slist[2].toInt();
        int notifyFrontend = slist[3].toInt();
        int oldrate = enc->SetSignalMonitoringRate(rate, notifyFrontend);
        retlist << QString::number(oldrate);
    }
    else if (command == "CHANGE_COLOUR")
    {
        bool up = slist[2].toInt(); 
        retlist << QString::number(enc->ChangeColour(up));
    }
    else if (command == "CHANGE_CONTRAST")
    {
        bool up = slist[2].toInt(); 
        retlist << QString::number(enc->ChangeContrast(up));
    }
    else if (command == "CHANGE_BRIGHTNESS")
    {
        bool up = slist[2].toInt(); 
        retlist << QString::number(enc->ChangeBrightness(up));
    }
    else if (command == "CHANGE_HUE")
    {
        bool up = slist[2].toInt(); 
        retlist << QString::number(enc->ChangeHue(up));
    }
    else if (command == "CHECK_CHANNEL")
    {
        QString name = slist[2];
        retlist << QString::number((int)(enc->CheckChannel(name)));
    }
    else if (command == "SHOULD_SWITCH_CARD")
    {
        QString chanid = slist[2];
        retlist << QString::number((int)(enc->ShouldSwitchToAnotherCard(chanid)));
    }
    else if (command == "CHECK_CHANNEL_PREFIX")
    {
        QString needed_spacer = QString::null;
        QString prefix        = slist[2];
        uint    is_complete_valid_channel_on_rec = 0;
        bool    is_extra_char_useful             = false;

        bool match = enc->CheckChannelPrefix(
            prefix, is_complete_valid_channel_on_rec,
            is_extra_char_useful, needed_spacer);

        retlist << QString::number((int)match);
        retlist << QString::number(is_complete_valid_channel_on_rec);
        retlist << QString::number((int)is_extra_char_useful);
        retlist << ((needed_spacer.isEmpty()) ? QString("X") : needed_spacer);
    }
    else if (command == "GET_NEXT_PROGRAM_INFO")
    {
        QString channelname = slist[2];
        QString chanid = slist[3];
        int direction = slist[4].toInt();
        QString starttime = slist[5];

        QString title = "", subtitle = "", desc = "", category = "";
        QString endtime = "", callsign = "", iconpath = "";
        QString seriesid = "", programid = "";

        enc->GetNextProgram(direction,
                            title, subtitle, desc, category, starttime,
                            endtime, callsign, iconpath, channelname, chanid,
                            seriesid, programid);

        if (title == "")
            title = " ";
        if (subtitle == "")
            subtitle = " ";
        if (desc == "")
            desc = " ";
        if (category == "")
            category = " ";
        if (starttime == "")
            starttime = " ";
        if (endtime == "")
            endtime = " ";
        if (callsign == "")
            callsign = " ";
        if (iconpath == "")
            iconpath = " ";
        if (channelname == "")
            channelname = " ";
        if (chanid == "")
            chanid = " ";
        if (seriesid == "")
            seriesid = " ";
        if (programid == "")
            programid = " ";

        retlist << title;
        retlist << subtitle;
        retlist << desc;
        retlist << category;
        retlist << starttime;
        retlist << endtime;
        retlist << callsign;
        retlist << iconpath;
        retlist << channelname;
        retlist << chanid;
        retlist << seriesid;
        retlist << programid;
    }
    else
    {
        VERBOSE(VB_IMPORTANT, QString("Unknown command: %1").arg(command));
        retlist << "ok";
    }

    SendResponse(pbssock, retlist);    
}

void MainServer::HandleRemoteEncoder(QStringList &slist, QStringList &commands,
                                     PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    int recnum = commands[1].toInt();

    QMap<int, EncoderLink *>::Iterator iter = encoderList->find(recnum);
    if (iter == encoderList->end())
    {
        VERBOSE(VB_IMPORTANT, QString("Unknown encoder: %1").arg(recnum));
        exit(BACKEND_BUGGY_EXIT_UNKNOWN_ENC);
    }

    EncoderLink *enc = iter.data();

    QString command = slist[1];

    QStringList retlist;

    if (command == "GET_STATE")
    {
        retlist << QString::number((int)enc->GetState());
    }
    if (command == "IS_BUSY")
    {
        retlist << QString::number((int)enc->IsBusy());
    }
    else if (command == "MATCHES_RECORDING")
    {
        ProgramInfo *pginfo = new ProgramInfo();
        pginfo->FromStringList(slist, 2);

        retlist << QString::number((int)enc->MatchesRecording(pginfo));

        delete pginfo;
    }
    else if (command == "START_RECORDING")
    {
        ProgramInfo *pginfo = new ProgramInfo();
        pginfo->FromStringList(slist, 2);
 
        retlist << QString::number(enc->StartRecording(pginfo));

        delete pginfo;
    }
    else if (command == "RECORD_PENDING")
    {
        ProgramInfo *pginfo = new ProgramInfo();
        int secsleft = slist[2].toInt();
        pginfo->FromStringList(slist, 3);

        enc->RecordPending(pginfo, secsleft);
 
        retlist << "OK";
        delete pginfo;
    }
    else if (command == "GET_MAX_BITRATE")
    {
        long long value = enc->GetMaxBitrate();
        encodeLongLong(retlist, value);
    }
    else if (command == "GET_CURRENT_RECORDING")
    {
        ProgramInfo *info = enc->GetRecording();
        info->ToStringList(retlist);
        delete info;
    }

    SendResponse(pbssock, retlist);
}

void MainServer::HandleIsActiveBackendQuery(QStringList &slist,
                                            PlaybackSock *pbs)
{
    QStringList retlist;
    QString queryhostname = slist[1];
    
    if (gContext->GetHostName() != queryhostname)
    {
        PlaybackSock *slave = getSlaveByHostname(queryhostname);
        if (slave != NULL)
        {
            retlist << "TRUE";
            slave->DownRef();
        }
        else
            retlist << "FALSE";
    }
    else
        retlist << "TRUE";

    SendResponse(pbs->getSocket(), retlist);
}

// Helper function for the guts of HandleCommBreakQuery + HandleCutListQuery
void MainServer::HandleCutMapQuery(const QString &chanid, 
                                   const QString &starttime,
                                   PlaybackSock *pbs, bool commbreak)
{
    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    QMap<long long, int> markMap;
    QMap<long long, int>::Iterator it;
    QDateTime startdt;
    startdt.setTime_t((uint)atoi(starttime));
    QStringList retlist;
    int rowcnt = 0;

    ProgramInfo *pginfo = ProgramInfo::GetProgramFromRecorded(chanid,
                                                              startdt);

    if (commbreak)
        pginfo->GetCommBreakList(markMap);
    else
        pginfo->GetCutList(markMap);

    for (it = markMap.begin(); it != markMap.end(); ++it)
    {
        rowcnt++;
        QString intstr = QString("%1").arg(it.data());
        retlist << intstr;
        encodeLongLong(retlist,it.key());
    }

    if (rowcnt > 0)
        retlist.prepend(QString("%1").arg(rowcnt));
    else
        retlist << "-1";

    if (pbssock)
        SendResponse(pbssock, retlist);

    return;
}

void MainServer::HandleCommBreakQuery(const QString &chanid,
                                      const QString &starttime,
                                      PlaybackSock *pbs)
{
// Commercial break query
// Format:  QUERY_COMMBREAK <chanid> <starttime>
// chanid is chanid, starttime is startime of prorgram in
//   # of seconds since Jan 1, 1970, in UTC time.  Same format as in
//   a ProgramInfo structure in a string list.
// Return structure is [number of rows] followed by a triplet of values:
//   each triplet : [type] [long portion 1] [long portion 2]
// type is the value in the map, right now 4 = commbreak start, 5= end
    return HandleCutMapQuery(chanid, starttime, pbs, true);
}

void MainServer::HandleCutlistQuery(const QString &chanid,
                                    const QString &starttime,
                                    PlaybackSock *pbs)
{
// Cutlist query
// Format:  QUERY_CUTLIST <chanid> <starttime>
// chanid is chanid, starttime is startime of prorgram in
//   # of seconds since Jan 1, 1970, in UTC time.  Same format as in
//   a ProgramInfo structure in a string list.
// Return structure is [number of rows] followed by a triplet of values:
//   each triplet : [type] [long portion 1] [long portion 2]
// type is the value in the map, right now 0 = commbreak start, 1 = end
    return HandleCutMapQuery(chanid, starttime, pbs, false);
}


void MainServer::HandleBookmarkQuery(const QString &chanid,
                                     const QString &starttime,
                                     PlaybackSock *pbs)
// Bookmark query
// Format:  QUERY_BOOKMARK <chanid> <starttime>
// chanid is chanid, starttime is startime of prorgram in
//   # of seconds since Jan 1, 1970, in UTC time.  Same format as in
//   a ProgramInfo structure in a string list.
// Return value is a long-long encoded as two separate values
{
    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    QDateTime startdt;
    startdt.setTime_t((uint)atoi(starttime));
    QStringList retlist;
    long long bookmark;

    ProgramInfo *pginfo = ProgramInfo::GetProgramFromRecorded(chanid,
                                                              startdt);
    bookmark = pginfo->GetBookmark();

    encodeLongLong(retlist,bookmark);

    if (pbssock)
        SendResponse(pbssock, retlist);

    return;
}


void MainServer::HandleSetBookmark(QStringList &tokens,
                                   PlaybackSock *pbs)
{
// Bookmark query
// Format:  SET_BOOKMARK <chanid> <starttime> <long part1> <long part2>
// chanid is chanid, starttime is startime of prorgram in
//   # of seconds since Jan 1, 1970, in UTC time.  Same format as in
//   a ProgramInfo structure in a string list.  The two longs are the two
//   portions of the bookmark value to set.

    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    QString chanid = tokens[1];
    QString starttime = tokens[2];
    QStringList bookmarklist;
    bookmarklist << tokens[3];
    bookmarklist << tokens[4];

    QDateTime startdt;
    startdt.setTime_t((uint)atoi(starttime));
    QStringList retlist;
    long long bookmark = decodeLongLong(bookmarklist, 0);

    ProgramInfo *pginfo = ProgramInfo::GetProgramFromRecorded(chanid,
                                                              startdt);
    pginfo->SetBookmark(bookmark);

    retlist << "OK";
    if (pbssock)
        SendResponse(pbssock, retlist);

    return;
}

void MainServer::HandleSettingQuery(QStringList &tokens, PlaybackSock *pbs)
{
// Format: QUERY_SETTING <hostname> <setting>
// Returns setting value as a string

    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    QString hostname = tokens[1];
    QString setting = tokens[2];
    QStringList retlist;

    QString retvalue = gContext->GetSettingOnHost(setting, hostname, "-1");

    retlist << retvalue;
    if (pbssock)
        SendResponse(pbssock, retlist);

    return;
}

void MainServer::HandleSetSetting(QStringList &tokens,
                                  PlaybackSock *pbs)
{
// Format: SET_SETTING <hostname> <setting> <value>
    QSocket *pbssock = NULL;
    if (pbs)
        pbssock = pbs->getSocket();

    QString hostname = tokens[1];
    QString setting = tokens[2];
    QString svalue = tokens[3];
    QStringList retlist;

    gContext->SaveSettingOnHost(setting, svalue, hostname);

    retlist << "OK";
    if (pbssock)
        SendResponse(pbssock, retlist);

    return;
}

void MainServer::HandleFileTransferQuery(QStringList &slist,
                                         QStringList &commands,
                                         PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    int recnum = commands[1].toInt();

    FileTransfer *ft = getFileTransferByID(recnum);
    if (!ft)
    {
        VERBOSE(VB_IMPORTANT, QString("Unknown file transfer socket: %1")
                               .arg(recnum));
        exit(BACKEND_BUGGY_EXIT_UNKNOWN_FILE_SOCK);
    }

    QString command = slist[1];

    QStringList retlist;

    if (command == "IS_OPEN")
    {
        bool isopen = ft->isOpen();

        retlist << QString::number(isopen);
    }
    else if (command == "DONE")
    {
        ft->Stop();
        retlist << "ok";
    }
    else if (command == "REQUEST_BLOCK")
    {
        int size = slist[2].toInt();

        retlist << QString::number(ft->RequestBlock(size));
    }
    else if (command == "SEEK")
    {
        long long pos = decodeLongLong(slist, 2);
        int whence = slist[4].toInt();
        long long curpos = decodeLongLong(slist, 5);

        long long ret = ft->Seek(curpos, pos, whence);
        encodeLongLong(retlist, ret);
    }
    else 
    {
        VERBOSE(VB_IMPORTANT, QString("Unknown command: %1").arg(command));
        retlist << "ok";
    }

    SendResponse(pbssock, retlist);
}

void MainServer::HandleGetRecorderNum(QStringList &slist, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    int retval = -1;

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    EncoderLink *encoder = NULL;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if (elink->IsConnected() && elink->MatchesRecording(pginfo))
        {
            retval = iter.key();
            encoder = elink;
        }
    }
    
    QStringList strlist = QString::number(retval);

    if (encoder)
    {
        if (encoder->IsLocal())
        {
            strlist << gContext->GetSetting("BackendServerIP");
            strlist << gContext->GetSetting("BackendServerPort");
        }
        else
        {
            strlist << gContext->GetSettingOnHost("BackendServerIP",
                                                  encoder->GetHostName(),
                                                  "nohostname");
            strlist << gContext->GetSettingOnHost("BackendServerPort",
                                                  encoder->GetHostName(), "-1");
        }
    }
    else
    {
        strlist << "nohost";
        strlist << "-1";
    }

    SendResponse(pbssock, strlist);    
    delete pginfo;
}

void MainServer::HandleGetRecorderFromNum(QStringList &slist, 
                                          PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    int recordernum = slist[1].toInt();
    EncoderLink *encoder = NULL;
    QStringList strlist;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->find(recordernum);

    if (iter != encoderList->end())
        encoder =  iter.data();

    if (encoder && encoder->IsConnected())
    {
        if (encoder->IsLocal())
        {
            strlist << gContext->GetSetting("BackendServerIP");
            strlist << gContext->GetSetting("BackendServerPort");
        }
        else
        {
            strlist << gContext->GetSettingOnHost("BackendServerIP",
                                                  encoder->GetHostName(),
                                                  "nohostname");
            strlist << gContext->GetSettingOnHost("BackendServerPort",
                                                  encoder->GetHostName(), "-1");
        }
    }
    else
    {
        strlist << "nohost";
        strlist << "-1";
    }

    SendResponse(pbssock, strlist);
}

void MainServer::HandleMessage(QStringList &slist, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    QString message = slist[1];

    MythEvent me(message);
    gContext->dispatch(me);

    QStringList retlist = "OK";

    SendResponse(pbssock, retlist);
}

void MainServer::HandleIsRecording(QStringList &slist, PlaybackSock *pbs)
{
    (void)slist;

    QSocket *pbssock = pbs->getSocket();
    int RecordingsInProgress = 0;
    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();
        if (elink->IsBusyRecording())
            RecordingsInProgress++;
    }

    QStringList retlist = QString::number(RecordingsInProgress);

    SendResponse(pbssock, retlist);
}

void MainServer::HandleGenPreviewPixmap(QStringList &slist, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    if (ismaster && pginfo->hostname != gContext->GetHostName())
    {
        PlaybackSock *slave = getSlaveByHostname(pginfo->hostname);

        if (!slave)
            VERBOSE(VB_IMPORTANT, "MainServer::HandleGenPreviewPixmap()"
                    "\n\t\t\tCouldn't find backend for: " +
                    QString("\n\t\t\t%1 : \"%2\"")
                    .arg(pginfo->title).arg(pginfo->subtitle));
        else
        {
            slave->GenPreviewPixmap(pginfo);
            slave->DownRef();
        }

        QStringList outputlist = "OK";
        SendResponse(pbssock, outputlist);
        delete pginfo;
        return;
    }

    if (pginfo->hostname != gContext->GetHostName())
    {
        VERBOSE(VB_IMPORTANT, "Mainserver, Error: Got request to generate " +
                QString("\n\t\t\tpreview pixmap for %1 on host %2.")
                .arg(pginfo->hostname).arg(gContext->GetHostName()));

        QStringList outputlist = "BAD";
        SendResponse(pbssock, outputlist);
        delete pginfo;
        return;
    }

    QUrl qurl = pginfo->pathname;
    QString filename = qurl.path();

    if (qurl.host() != "")
        filename = LocalFilePath(qurl);

    EncoderLink *elink = NULL;
    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();

    for (; iter != encoderList->end(); ++iter)
    {
        if (iter.data()->IsLocal())
            elink = iter.data();
    }

    if (elink == NULL)
    {
        VERBOSE(VB_IMPORTANT, QString("Couldn't find EncoderLink when trying to "
                                "generate preview pixmap for: %1 : %2")
                               .arg(pginfo->title).arg(pginfo->subtitle));
        QStringList outputlist = "BAD";
        SendResponse(pbssock, outputlist);
        delete pginfo;
        return;
    }

    int len = 0;
    int width = 0, height = 0;
    float aspect = 0;
    int secondsin = gContext->GetNumSetting("PreviewPixmapOffset", 64) +
                    gContext->GetNumSetting("RecordPreRoll",0);

    unsigned char *data = (unsigned char *)elink->GetScreenGrab(
        pginfo, filename, secondsin, len, width, height, aspect);

    if (PreviewGenerator::SavePreview(filename+".png", data,
                                      width, height, aspect))
    {
        QStringList retlist = "OK";
        SendResponse(pbssock, retlist);    
    }
    else
    {
        VERBOSE(VB_IMPORTANT, "MainServer: Failed to make preview image.");
        QStringList outputlist = "BAD";
        SendResponse(pbssock, outputlist);
    } 

    delete [] data;
    delete pginfo;
}

void MainServer::HandlePixmapLastModified(QStringList &slist, PlaybackSock *pbs)
{
    QSocket *pbssock = pbs->getSocket();

    ProgramInfo *pginfo = new ProgramInfo();
    pginfo->FromStringList(slist, 1);

    QDateTime lastmodified;
    QStringList strlist;
    Qt::DateFormat f = Qt::TextDate;

    if (ismaster && pginfo->hostname != gContext->GetHostName())
    {
        PlaybackSock *slave = getSlaveByHostname(pginfo->hostname);

        if (slave) 
        {
             QString slavetime = slave->PixmapLastModified(pginfo);
             slave->DownRef();

             strlist += slavetime;
             SendResponse(pbssock, strlist);
             delete pginfo;
             return;
        }
        else
        {
            VERBOSE(VB_IMPORTANT, "MainServer::HandlePixmapLastModified()"
                    "\n\t\t\t Couldn't find backend for: " +
                    QString("\n\t\t\t%1 : \"%2\"").arg(pginfo->title)
                    .arg(pginfo->subtitle));
        }
    }

    if (pginfo->hostname != gContext->GetHostName())
    {
        VERBOSE(VB_IMPORTANT, QString("Got requested for last modified date and time "
                                "of preview pixmap on %1")
                                .arg(pginfo->hostname));

        QStringList outputlist = "BAD";
        SendResponse(pbssock, outputlist);
        delete pginfo;
        return;
    }

    QUrl qurl = pginfo->pathname;
    QString filename = qurl.path();

    if (qurl.host() != "")
        filename = LocalFilePath(qurl);

    filename += ".png";

    QFileInfo finfo(filename);

    if (finfo.exists())
    {
        lastmodified = finfo.lastModified();
        strlist = lastmodified.toString(f);
    }
    else
        strlist = "BAD";   
 
    SendResponse(pbssock, strlist);
    delete pginfo;
}

void MainServer::HandleBackendRefresh(QSocket *socket)
{
    gContext->RefreshBackendConfig();

    QStringList retlist = "OK";
    SendResponse(socket, retlist);    
}

void MainServer::HandleBlockShutdown(bool blockShutdown, PlaybackSock *pbs)
{            
    pbs->setBlockShutdown(blockShutdown);
    
    QSocket *socket = pbs->getSocket();
    QStringList retlist = "OK";
    SendResponse(socket, retlist);    
}

void MainServer::deferredDeleteSlot(void)
{
    QMutexLocker lock(&deferredDeleteLock);

    if (deferredDeleteList.size() == 0)
        return;

    DeferredDeleteStruct dds = deferredDeleteList.front();
    while (dds.ts.secsTo(QDateTime::currentDateTime()) > 30)
    {
        delete dds.sock;
        deferredDeleteList.pop_front();
        if (deferredDeleteList.size() == 0)
            return;
        dds = deferredDeleteList.front();
    }
}

void MainServer::DeletePBS(PlaybackSock *sock)
{
    DeferredDeleteStruct dds;
    dds.sock = sock;
    dds.ts = QDateTime::currentDateTime();

    QMutexLocker lock(&deferredDeleteLock);
    deferredDeleteList.push_back(dds);
}

void MainServer::endConnection(RefSocket *socket)
{
    vector<PlaybackSock *>::iterator it = playbackList.begin();
    for (; it != playbackList.end(); ++it)
    {
        PlaybackSock *pbs = (*it);
        QSocket *sock = pbs->getSocket();
        if (sock == socket)
        {
            if (ismaster && pbs->isSlaveBackend())
            {
                VERBOSE(VB_IMPORTANT,QString("Slave backend: %1 no longer connected")
                                       .arg(pbs->getHostname()));

                QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
                for (; iter != encoderList->end(); ++iter)
                {
                    EncoderLink *elink = iter.data();
                    if (elink->GetSocket() == pbs)
                    {
                        elink->SetSocket(NULL);
                        if (m_sched)
                            m_sched->SlaveDisconnected(elink->GetCardID());
                    }
                }
                if (m_sched)
                    m_sched->Reschedule(0);

                QString message = QString("LOCAL_SLAVE_BACKEND_OFFLINE %2")
                                          .arg(pbs->getHostname());
                MythEvent me(message);
                gContext->dispatch(me);
            }

            LiveTVChain *chain;
            if ((chain = GetExistingChain(sock)))
            {
                chain->DelHostSocket(sock);
                if (chain->HostSocketCount() == 0)
                {
                    QMap<int, EncoderLink *>::iterator i = encoderList->begin();
                    for (; i != encoderList->end(); ++i)
                    {
                        EncoderLink *enc = i.data();
                        if (enc->IsLocal())
                        {
                            while (enc->GetState() == kState_ChangingState)
                                usleep(500);

                            if (enc->IsBusy() && 
                                enc->GetChainID() == chain->GetID())
                            {
                                enc->StopLiveTV();
                            }
                        }
                    }
                    DeleteChain(chain);
                }
            }

            pbs->SetDisconnected();
            playbackList.erase(it);

            PlaybackSock *testsock = getPlaybackBySock(socket);
            if (testsock)
                VERBOSE(VB_IMPORTANT, "Playback sock still exists?");

            pbs->DownRef();
            return;
        }
    }

    vector<FileTransfer *>::iterator ft = fileTransferList.begin();
    for (; ft != fileTransferList.end(); ++ft)
    {
        QSocket *sock = (*ft)->getSocket();
        if (sock == socket)
        {
            socket->DownRef();
            delete (*ft);
            fileTransferList.erase(ft);
            return;
        }
    }

    VERBOSE(VB_IMPORTANT, "Unknown socket closing");
}

PlaybackSock *MainServer::getSlaveByHostname(QString &hostname)
{
    if (!ismaster)
        return NULL;

    vector<PlaybackSock *>::iterator iter = playbackList.begin();
    for (; iter != playbackList.end(); iter++)
    {
        PlaybackSock *pbs = (*iter);
        if (pbs->isSlaveBackend() && 
            ((pbs->getHostname() == hostname) || (pbs->getIP() == hostname)))
        {
            pbs->UpRef();
            return pbs;
        }
    }

    return NULL;
}

PlaybackSock *MainServer::getPlaybackBySock(QSocket *sock)
{
    PlaybackSock *retval = NULL;

    vector<PlaybackSock *>::iterator it = playbackList.begin();
    for (; it != playbackList.end(); ++it)
    {
        if (sock == (*it)->getSocket())
        {
            retval = (*it);
            break;
        }
    }

    return retval;
}

FileTransfer *MainServer::getFileTransferByID(int id)
{
    FileTransfer *retval = NULL;

    vector<FileTransfer *>::iterator it = fileTransferList.begin();
    for (; it != fileTransferList.end(); ++it)
    {
        if (id == (*it)->getSocket()->socket())
        {
            retval = (*it);
            break;
        }
    }

    return retval;
}

FileTransfer *MainServer::getFileTransferBySock(QSocket *sock)
{
    FileTransfer *retval = NULL;

    vector<FileTransfer *>::iterator it = fileTransferList.begin();
    for (; it != fileTransferList.end(); ++it)
    {
        if (sock == (*it)->getSocket())
        {
            retval = (*it);
            break;
        }
    }

    return retval;
}

LiveTVChain *MainServer::GetExistingChain(QString id)
{
    QMutexLocker lock(&liveTVChainsLock);

    LiveTVChain *chain;

    QPtrListIterator<LiveTVChain> it(liveTVChains);
    while ((chain = it.current()) != 0)
    {
        ++it;
        if (chain->GetID() == id)
            return chain;
    }

    return NULL;
}

LiveTVChain *MainServer::GetExistingChain(QSocket *sock)
{
    QMutexLocker lock(&liveTVChainsLock);

    LiveTVChain *chain;

    QPtrListIterator<LiveTVChain> it(liveTVChains);
    while ((chain = it.current()) != 0)
    {
        ++it;
        if (chain->IsHostSocket(sock))
            return chain;
    }

    return NULL;
}

LiveTVChain *MainServer::GetChainWithRecording(ProgramInfo *pginfo)
{
    QMutexLocker lock(&liveTVChainsLock);

    LiveTVChain *chain;

    QPtrListIterator<LiveTVChain> it(liveTVChains);
    while ((chain = it.current()) != 0)
    {
        ++it;
        if (chain->ProgramIsAt(pginfo) >= 0)
            return chain;
    }

    return NULL;
}

void MainServer::AddToChains(LiveTVChain *chain)
{
    liveTVChains.append(chain);
}

void MainServer::DeleteChain(LiveTVChain *chain)
{
    QMutexLocker lock(&liveTVChainsLock);

    while (liveTVChains.removeRef(chain))
        ;

    delete chain;
}

QString MainServer::LocalFilePath(QUrl &url)
{
    QString lpath = url.path();

    if (lpath.section('/', -2, -2) == "channels")
    {
        // This must be an icon request. Check channel.icon to be safe.
        QString querytext;

        QString file = lpath.section('/', -1);
        lpath = "";

        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT icon FROM channel WHERE icon LIKE :FILENAME ;");
        query.bindValue(":FILENAME", QString("%") + file);

        if (query.exec() && query.isActive() && query.size())
        {
            query.next();
            lpath = query.value(0).toString();
        }
        else
        {
            MythContext::DBError("Icon path", query);
        }
    }
    else
    {
        lpath = lpath.section('/', -1);
        lpath = gContext->GetFilePrefix() + "/" + lpath;
    }
    VERBOSE(VB_FILE, QString("Local file path: %1").arg(lpath));

    return lpath;
}

void MainServer::masterServerDied(void)
{
    bool deleted = false;

    vector<PlaybackSock *>::iterator it = playbackList.begin();
    for (; it != playbackList.end(); it++)
    {
        PlaybackSock *pbs = (*it);
        if (pbs == masterServer)
        {
            playbackList.erase(it);
            deleted = true;
            break;
        }
    }

    if (!deleted)
        VERBOSE(VB_IMPORTANT, "Unable to find master server connection in pbs list.");
    
    masterServer->DownRef();
    masterServer = NULL;
    masterServerReconnect->start(1000, true);
}

void MainServer::reconnectTimeout(void)
{
    RefSocket *masterServerSock = new RefSocket();

    QString server = gContext->GetSetting("MasterServerIP", "localhost");
    int port = gContext->GetNumSetting("MasterServerPort", 6543);

    VERBOSE(VB_IMPORTANT, QString("Connecting to master server: %1:%2")
                           .arg(server).arg(port));

    masterServerSock->connectToHost(server, port);

    int num = 0;
    while (masterServerSock->state() == QSocket::HostLookup ||
           masterServerSock->state() == QSocket::Connecting)
    {
        qApp->processEvents();
        usleep(50);
        num++;
        if (num > 100)
        {
            VERBOSE(VB_IMPORTANT, "Connection to master server timed out.");
            masterServerReconnect->start(1000, true);
            masterServerSock->DownRef();
            return;
        }
    }

    if (masterServerSock->state() != QSocket::Connected)
    {
        VERBOSE(VB_IMPORTANT, "Could not connect to master server.");
        masterServerReconnect->start(1000, true);
        masterServerSock->DownRef();
        return;
    }

    VERBOSE(VB_IMPORTANT, "Connected successfully");

    QString str = QString("ANN SlaveBackend %1 %2")
                          .arg(gContext->GetHostName())
                          .arg(gContext->GetSetting("BackendServerIP"));

    masterServerSock->Lock();

    QStringList strlist = str;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();
    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();
        elink->CancelNextRecording(true);
        ProgramInfo *pinfo = elink->GetRecording();
        pinfo->ToStringList(strlist);
        delete pinfo;
    }

    WriteStringList(masterServerSock, strlist);
    ReadStringList(masterServerSock, strlist);

    connect(masterServerSock, SIGNAL(readyRead()), this, SLOT(readSocket()));
    connect(masterServerSock, SIGNAL(connectionClosed()), this, 
            SLOT(masterServerDied()));

    masterServer = new PlaybackSock(this, masterServerSock, server, true);
    playbackList.push_back(masterServer);

    masterServerSock->Unlock();

    // Handle any messages sent before the readyRead signal was connected.
    ProcessRequest(masterServerSock);

    AutoExpire::Update(encoderList, false);
}

// returns true, if a client (slavebackends are not counted!)
// is connected by checking the lists.
bool MainServer::isClientConnected()
{
    bool foundClient = false;

    foundClient |= (fileTransferList.size() > 0);

    if ((playbackList.size() > 0) && !foundClient)
    {
        vector<PlaybackSock *>::iterator it = playbackList.begin();
        for (; !foundClient && (it != playbackList.end()); ++it)
        {
            // we simply ignore slaveBackends!
            // and clients that don't want to block shutdown
            if (!(*it)->isSlaveBackend() && (*it)->getBlockShutdown())
                foundClient = true;
        }
    }
    
    return (foundClient);
}

// sends the Slavebackends the request to shut down using haltcmd
void MainServer::ShutSlaveBackendsDown(QString &haltcmd)
{
    QStringList bcast = "SHUTDOWN_NOW";
    bcast << haltcmd;
    
    if (playbackList.size() > 0)
    {
        vector<PlaybackSock *>::iterator it = playbackList.begin();
        for (; it != playbackList.end(); ++it)
        {
            if ((*it)->isSlaveBackend())
                WriteStringList((*it)->getSocket(),bcast); 
        }
    }
}

void MainServer::FillStatusXML( QDomDocument *pDoc )
{
    QString   dateFormat   = gContext->GetSetting("DateFormat", "M/d/yyyy");
    if (dateFormat.find(QRegExp("yyyy")) < 0)
        dateFormat += " yyyy";
    QString   shortdateformat = gContext->GetSetting("ShortDateFormat", "M/d");
    QString   timeformat      = gContext->GetSetting("TimeFormat", "h:mm AP");
    QDateTime qdtNow          = QDateTime::currentDateTime();

    // Add Root Node.

    QDomElement root = pDoc->createElement("Status");
    pDoc->appendChild(root);

    root.setAttribute("date"    , qdtNow.toString(dateFormat));
    root.setAttribute("time"    , qdtNow.toString(timeformat)   );
    root.setAttribute("version" , MYTH_BINARY_VERSION           );
    root.setAttribute("protoVer", MYTH_PROTO_VERSION            );

    // Add all encoders, if any

    QDomElement encoders = pDoc->createElement("Encoders");
    root.appendChild(encoders);

    int  numencoders = 0;
    bool isLocal     = true;

    QMap<int, EncoderLink *>::Iterator iter = encoderList->begin();

    for (; iter != encoderList->end(); ++iter)
    {
        EncoderLink *elink = iter.data();

        if (elink != NULL)
        {
            isLocal = elink->IsLocal();

            QDomElement encoder = pDoc->createElement("Encoder");
            encoders.appendChild(encoder);

            encoder.setAttribute("id"            , elink->GetCardID()       );
            encoder.setAttribute("local"         , isLocal                  );
            encoder.setAttribute("connected"     , elink->IsConnected()     );
            encoder.setAttribute("state"         , elink->GetState()        );
            //encoder.setAttribute("lowOnFreeSpace", elink->isLowOnFreeSpace());

            if (isLocal)
                encoder.setAttribute("hostname", gContext->GetHostName());
            else
                encoder.setAttribute("hostname", elink->GetHostName());

            if (elink->IsConnected())
                numencoders++;

            switch (elink->GetState())
            {
                case kState_WatchingLiveTV:
                case kState_RecordingOnly:
                case kState_WatchingRecording:
                {
                    if (isLocal)
                    {
                        ProgramInfo *pInfo = elink->GetRecording();

                        if (pInfo)
                        {
                            FillProgramInfo(pDoc, encoder, pInfo);
                            delete pInfo;
                        }
                    }

                    break;
                }

                default:
                    break;
            }
        }
    }

    encoders.setAttribute("count", numencoders);

    // Add upcoming shows

    QDomElement scheduled = pDoc->createElement("Scheduled");
    root.appendChild(scheduled);

    list<ProgramInfo *> recordingList;

    if (m_sched)
        m_sched->getAllPending(&recordingList);

    unsigned int iNum = 10;
    unsigned int iNumRecordings = 0;

    list<ProgramInfo *>::iterator itProg = recordingList.begin();
    for (; (itProg != recordingList.end()) && iNumRecordings < iNum; itProg++)
    {
        if (((*itProg)->recstatus  <= rsWillRecord) &&
            ((*itProg)->recstartts >= QDateTime::currentDateTime()))
        {
            iNumRecordings++;
            FillProgramInfo(pDoc, scheduled, *itProg);
        }
    }

    while (recordingList.size() > 0)
    {
        ProgramInfo *pginfo = recordingList.back();
        delete pginfo;
        recordingList.pop_back();
    }

    scheduled.setAttribute("count", iNumRecordings);

    // Add Job Queue Entries

    QDomElement jobqueue = pDoc->createElement("JobQueue");
    root.appendChild(jobqueue);

    QMap<int, JobQueueEntry> jobs;
    QMap<int, JobQueueEntry>::Iterator it;

    JobQueue::GetJobsInQueue(jobs,
                             JOB_LIST_NOT_DONE | JOB_LIST_ERROR |
                             JOB_LIST_RECENT);

    if (jobs.size())
    {
        for (it = jobs.begin(); it != jobs.end(); ++it)
        {
            ProgramInfo *pInfo;

            pInfo = ProgramInfo::GetProgramFromRecorded(it.data().chanid,
                                                        it.data().starttime);

            if (!pInfo)
                continue;

            QDomElement job = pDoc->createElement("Job");
            jobqueue.appendChild(job);

            job.setAttribute("id"        , it.data().id         );
            job.setAttribute("chanId"    , it.data().chanid     );
            job.setAttribute("startTime" , it.data().starttime.toTime_t());
            job.setAttribute("startTs"   , it.data().startts    );
            job.setAttribute("insertTime", it.data().inserttime.toTime_t());
            job.setAttribute("type"      , it.data().type       );
            job.setAttribute("cmds"      , it.data().cmds       );
            job.setAttribute("flags"     , it.data().flags      );
            job.setAttribute("status"    , it.data().status     );
            job.setAttribute("statusTime", it.data().statustime.toTime_t());
            job.setAttribute("args"      , it.data().args       );

            if (it.data().hostname == "")
                job.setAttribute("hostname", QObject::tr("master"));
            else
                job.setAttribute("hostname",it.data().hostname);

            QDomText textNode = pDoc->createTextNode(it.data().comment);
            job.appendChild(textNode);

            FillProgramInfo(pDoc, job, pInfo);

            delete pInfo;
        }
    }

    jobqueue.setAttribute( "count", jobs.size() );

    // Add Machine information

    QDomElement mInfo   = pDoc->createElement("MachineInfo");
    QDomElement storage = pDoc->createElement("Storage"    );
    QDomElement load    = pDoc->createElement("Load"       );
    QDomElement guide   = pDoc->createElement("Guide"      );

    root.appendChild (mInfo  );
    mInfo.appendChild(storage);
    mInfo.appendChild(load   );
    mInfo.appendChild(guide  );

    // drive space   ---------------------

    long long iTotal = -1, iUsed = -1, iAvail = -1;

    iAvail = getDiskSpace(recordfileprefix, iTotal, iUsed);

    storage.setAttribute("total", (int)(iTotal>>10));
    storage.setAttribute("used" , (int)(iUsed>>10));
    storage.setAttribute("free" , (int)(iAvail>>10));

    // load average ---------------------

    double rgdAverages[3];

    if (getloadavg(rgdAverages, 3) != -1)
    {
        load.setAttribute("avg1", rgdAverages[0]);
        load.setAttribute("avg2", rgdAverages[1]);
        load.setAttribute("avg3", rgdAverages[2]);
    }

    // Guide Data ---------------------

    QDateTime GuideDataThrough;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare("SELECT max(endtime) FROM program;");

    if (query.exec() && query.isActive() && query.size())
    {
        query.next();

        if (query.isValid())
            GuideDataThrough = QDateTime::fromString(query.value(0).toString(),
                                                     Qt::ISODate);
    }

    guide.setAttribute("start", gContext->GetSetting("mythfilldatabaseLastRunStart"));
    guide.setAttribute("end", gContext->GetSetting("mythfilldatabaseLastRunEnd"));
    guide.setAttribute("status", gContext->GetSetting("mythfilldatabaseLastRunStatus"));
    if (gContext->GetNumSetting("MythFillGrabberSuggestsTime", 0))
    {
        guide.setAttribute("next",
            gContext->GetSetting("MythFillSuggestedRunTime"));
    }

    if (!GuideDataThrough.isNull())
    {
        guide.setAttribute("guideThru", QDateTime(GuideDataThrough).toString("yyyy-MM-dd hh:mm"));
        guide.setAttribute("guideDays", qdtNow.daysTo(GuideDataThrough));
    }

    QDomText dataDirectMessage = pDoc->createTextNode(gContext->GetSetting("DataDirectMessage"));
    guide.appendChild(dataDirectMessage);
}

void MainServer::FillProgramInfo(QDomDocument *pDoc, QDomElement &e, 
                                 ProgramInfo *pInfo)
{
    if ((pDoc == NULL) || (pInfo == NULL))
        return;

    // Build Program Element

    QDomElement program = pDoc->createElement( "Program" );
    e.appendChild( program );

    program.setAttribute( "seriesId"    , pInfo->seriesid     );
    program.setAttribute( "programId"   , pInfo->programid    );
    program.setAttribute( "title"       , pInfo->title        );
    program.setAttribute( "subTitle"    , pInfo->subtitle     );
    program.setAttribute( "category"    , pInfo->category     );
    program.setAttribute( "catType"     , pInfo->catType      );
    program.setAttribute( "startTime"   , pInfo->startts.toTime_t());
    program.setAttribute( "endTime"     , pInfo->endts.toTime_t());
    program.setAttribute( "repeat"      , pInfo->repeat       );
    program.setAttribute( "stars"       , pInfo->stars        );
//    program.setAttribute( "fileSize"    , pInfo->filesize );
    program.setAttribute( "lastModified", pInfo->lastmodified.toTime_t() );
    program.setAttribute( "programFlags", pInfo->programflags );
    program.setAttribute( "hostname"    , pInfo->hostname     );

    if (pInfo->hasAirDate)
        program.setAttribute( "airdate"  , pInfo->originalAirDate.toString() );

    QDomText textNode = pDoc->createTextNode( pInfo->description );
    program.appendChild( textNode );

    // Build Channel Child Element

    QDomElement channel = pDoc->createElement( "Channel" );
    program.appendChild( channel );

    channel.setAttribute( "chanId"     , pInfo->chanid      );
    channel.setAttribute( "callSign"   , pInfo->chansign    );
//    channel.setAttribute( "iconPath"   , pInfo->iconpath    );
    channel.setAttribute( "channelName", pInfo->channame    );
    channel.setAttribute( "chanFilters", pInfo->chanOutputFilters );
    channel.setAttribute( "sourceId"   , pInfo->sourceid    );
    channel.setAttribute( "inputId"    , pInfo->inputid     );

    // Build Recording Child Element

    QDomElement recording = pDoc->createElement( "Recording" );
    program.appendChild( recording );

    recording.setAttribute( "recordId"      , pInfo->recordid    );
    recording.setAttribute( "recStartTs"    , pInfo->recstartts.toTime_t());
    recording.setAttribute( "recEndTs"      , pInfo->recendts.toTime_t());
    recording.setAttribute( "recStatus"     , pInfo->recstatus   );
    recording.setAttribute( "recPriority"   , pInfo->recpriority );
    recording.setAttribute( "recGroup"      , pInfo->recgroup    );
    recording.setAttribute( "playGroup"     , pInfo->playgroup   );
    recording.setAttribute( "recType"       , pInfo->rectype     );
    recording.setAttribute( "dupInType"     , pInfo->dupin       );
    recording.setAttribute( "dupMethod"     , pInfo->dupmethod   );

    recording.setAttribute( "encoderId"     , pInfo->cardid      );

    recording.setAttribute( "recProfile"    , pInfo->GetProgramRecordingProfile());

    recording.setAttribute( "preRollSeconds", gContext->GetNumSetting("RecordPreRoll", 0));
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */
