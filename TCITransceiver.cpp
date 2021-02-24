#include "TCITransceiver.hpp"

#include <QRegularExpression>
#include <QLocale>
#include <QThread>
#include <qmath.h>
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
#include <QRandomGenerator>
#endif

#include "commons.h"

#include "NetworkServerLookup.hpp"

#include "moc_TCITransceiver.cpp"

namespace
{
  char const * const TCI_transceiver_name {"TCI Client"};


  QString map_mode (Transceiver::MODE mode)
  {
    switch (mode)
      {
      case Transceiver::AM: return "am";
      case Transceiver::CW: return "cw";
//      case Transceiver::CW_R: return "CW-R";
      case Transceiver::USB: return "usb";
      case Transceiver::LSB: return "lsb";
//      case Transceiver::FSK: return "RTTY";
//      case Transceiver::FSK_R: return "RTTY-R";
      case Transceiver::DIG_L: return "digl";
      case Transceiver::DIG_U: return "digu";
      case Transceiver::FM: return "wfm";
      case Transceiver::DIG_FM:
        return "nfm";
      default: break;
      }
    return "";
  }
static const QString SmTZ(";");
static const QString SmDP(":");
static const QString SmCM(",");
static const QString SmTrue("true");
static const QString SmFalse("false");

// Command maps
static const QString CmdDevice("device");
static const QString CmdReceiveOnly("receive_only");
static const QString CmdTrxCount("trx_count");
static const QString CmdChannelCount("channels_count");
static const QString CmdVfoLimits("vfo_limits");
static const QString CmdIfLimits("if_limits");
static const QString CmdModeList("modulations_list");
static const QString CmdMode("modulation");
static const QString CmdReady("ready");
static const QString CmdStop("stop");
static const QString CmdStart("start");
static const QString CmdPreamp("preamp");
static const QString CmdDds("dds");
static const QString CmdIf("if");
static const QString CmdTrx("trx");
static const QString CmdRxEnable("rx_enable");
static const QString CmdTxEnable("tx_enable");
static const QString CmdRitEnable("rit_enable");
static const QString CmdRitOffset("rit_offset");
static const QString CmdXitEnable("xit_enable");
static const QString CmdXitOffset("xit_offset");
static const QString CmdSplitEnable("split_enable");
static const QString CmdIqSR("iq_samplerate");
static const QString CmdIqStart("iq_start");
static const QString CmdIqStop("iq_stop");
static const QString CmdCWSpeed("cw_macros_speed");
static const QString CmdCWDelay("cw_macros_delay");
static const QString CmdSpot("spot");
static const QString CmdSpotDelete("spot_delete");
static const QString CmdFilterBand("rx_filter_band");
static const QString CmdVFO("vfo");
static const QString CmdVersion("protocol"); //protocol:esdr,1.2;
static const QString CmdTune("tune");
static const QString CmdRxMute("rx_mute");
static const QString CmdSmeter("rx_smeter");
static const QString CmdPower("tx_power");
static const QString CmdSWR("tx_swr");
static const QString CmdECoderRX("ecoder_switch_rx");
static const QString CmdECoderVFO("ecoder_switch_channel");
static const QString CmdAudioSR("audio_samplerate");
static const QString CmdAudioStart("audio_start");
static const QString CmdAudioStop("audio_stop");
static const QString CmdAppFocus("app_focus");
static const QString CmdVolume("volume");
static const QString CmdSqlEnable("sql_enable");
static const QString CmdSqlLevel("sql_level");
static const QString CmdDrive("drive");
static const QString CmdTuneDrive("tune_drive");
static const QString CmdMute("mute");

}

extern "C" {
  void   fil4_(qint16*, qint32*, qint16*, qint32*, float*);
}
extern dec_data dec_data;

extern float gran();		// Noise generator (for tests only)

#define RAMP_INCREMENT 64  // MUST be an integral factor of 2^16

#if defined (WSJT_SOFT_KEYING)
# define SOFT_KEYING WSJT_SOFT_KEYING
#else
# define SOFT_KEYING 1
#endif

double constexpr TCITransceiver::m_twoPi;

void TCITransceiver::register_transceivers (TransceiverFactory::Transceivers * registry, unsigned id)
{
  (*registry)[TCI_transceiver_name] = TransceiverFactory::Capabilities {id, TransceiverFactory::Capabilities::tci, true};
}

static constexpr quint32 AudioHeaderSize = 16u*sizeof(quint32);

TCITransceiver::TCITransceiver (std::unique_ptr<TransceiverBase> wrapped,
                                                                QString const& address, bool use_for_ptt,
                                                                int poll_interval, QObject * parent)
  : PollingTransceiver {poll_interval, parent}
  , wrapped_ {std::move (wrapped)}
  , use_for_ptt_ {use_for_ptt}
  , server_ {address}
  , do_snr_ {(poll_interval & do__snr) == do__snr}
  , do_pwr_ {(poll_interval & do__pwr) == do__pwr}
  , rig_power_ {(poll_interval & rig__power) == rig__power}
  , rig_power_off_ {(poll_interval & rig__power_off) == rig__power_off}
  , commander_ {nullptr}
  , tci_timer1_ {nullptr}
  , tci_loop1_ {nullptr}
  , tci_timer2_ {nullptr}
  , tci_loop2_ {nullptr}
  , tci_timer3_ {nullptr}
  , tci_loop3_ {nullptr}
  , m_downSampleFactor {4}
  , m_buffer ((m_downSampleFactor > 1) ?
              new short [max_buffer_size * m_downSampleFactor] : nullptr)
  , m_quickClose {false}
  , m_phi {0.0}
  , m_toneSpacing {0.0}
  , m_fSpread {0.0}
  , m_state {Idle}
  , m_tuning {false}
  , m_cwLevel {false}
  , m_j0 {-1}
  , m_toneFrequency0 {1500.0}
  , debug_file_ {QDir(QStandardPaths::writableLocation (QStandardPaths::DataLocation)).absoluteFilePath ("jtdx_debug.txt").toStdString()}
{
    m_samplesPerFFT = 6912 / 2;
    m_period = 15.0;
    tci_Ready = false;
    trxA = 0;
    trxB = 0;
    cntIQ = 0;
    bIQ = false;
    inConnected = false;
    audioSampleRate = 48000u;
    mapCmd_[CmdDevice]       = Cmd_Device;
    mapCmd_[CmdReceiveOnly]  = Cmd_ReceiveOnly;
    mapCmd_[CmdTrxCount]     = Cmd_TrxCount;
    mapCmd_[CmdChannelCount] = Cmd_ChannelCount;
    mapCmd_[CmdVfoLimits]    = Cmd_VfoLimits;
    mapCmd_[CmdIfLimits]     = Cmd_IfLimits;
    mapCmd_[CmdModeList]     = Cmd_ModeList;
    mapCmd_[CmdMode]         = Cmd_Mode;
    mapCmd_[CmdReady]        = Cmd_Ready;
    mapCmd_[CmdStop]         = Cmd_Stop;
    mapCmd_[CmdStart]        = Cmd_Start;
    mapCmd_[CmdPreamp]       = Cmd_Preamp;
    mapCmd_[CmdDds]          = Cmd_Dds;
    mapCmd_[CmdIf]           = Cmd_If;
    mapCmd_[CmdTrx]          = Cmd_Trx;
    mapCmd_[CmdRxEnable]     = Cmd_RxEnable;
    mapCmd_[CmdTxEnable]     = Cmd_TxEnable;
    mapCmd_[CmdRitEnable]    = Cmd_RitEnable;
    mapCmd_[CmdRitOffset]    = Cmd_RitOffset;
    mapCmd_[CmdXitEnable]    = Cmd_XitEnable;
    mapCmd_[CmdXitOffset]    = Cmd_XitOffset;
    mapCmd_[CmdSplitEnable]  = Cmd_SplitEnable;
    mapCmd_[CmdIqSR]         = Cmd_IqSR;
    mapCmd_[CmdIqStart]      = Cmd_IqStart;
    mapCmd_[CmdIqStop]       = Cmd_IqStop;
    mapCmd_[CmdCWSpeed]      = Cmd_CWSpeed;
    mapCmd_[CmdCWDelay]      = Cmd_CWDelay;
    mapCmd_[CmdFilterBand]   = Cmd_FilterBand;
    mapCmd_[CmdVFO]          = Cmd_VFO;
    mapCmd_[CmdVersion]      = Cmd_Version;
    mapCmd_[CmdTune]         = Cmd_Tune;
    mapCmd_[CmdRxMute]       = Cmd_RxMute;
    mapCmd_[CmdSmeter]       = Cmd_Smeter;
    mapCmd_[CmdPower]        = Cmd_Power;
    mapCmd_[CmdSWR]          = Cmd_SWR;
    mapCmd_[CmdECoderRX]     = Cmd_ECoderRX;
    mapCmd_[CmdECoderVFO]    = Cmd_ECoderVFO;
    mapCmd_[CmdAudioSR]      = Cmd_AudioSR;
    mapCmd_[CmdAudioStart]   = Cmd_AudioStart;
    mapCmd_[CmdAudioStop]    = Cmd_AudioStop;
    mapCmd_[CmdAppFocus]     = Cmd_AppFocus;
    mapCmd_[CmdVolume]       = Cmd_Volume;
    mapCmd_[CmdSqlEnable]    = Cmd_SqlEnable;
    mapCmd_[CmdSqlLevel]     = Cmd_SqlLevel;
    mapCmd_[CmdDrive]        = Cmd_Drive;
    mapCmd_[CmdTuneDrive]    = Cmd_TuneDrive;
    mapCmd_[CmdMute]         = Cmd_Mute;
}

void TCITransceiver::onConnected()
{
    inConnected = true;
//    printf("%s(%0.1f) TCI connected\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
}

void TCITransceiver::onDisconnected()
{
    inConnected = false;
//    printf("%s(%0.1f) TCI disconnected\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
}


void TCITransceiver::onError(QAbstractSocket::SocketError err)
{
//qDebug() << "WebInThread::onError";
//    printf("%s(%0.1f) TCI error:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),err);
    throw error {tr ("TCI websocket error")+QString(err)};
}

int TCITransceiver::do_start (JTDXDateTime * jtdxtime)
{
//  QThread::currentThread()->setPriority(QThread::HighPriority);
//  if (tci_Ready) do_stop()
//  printf("do_start tci_Ready:%d\n",tci_Ready);
  TRACE_CAT ("TCITransceiver", "starting");
  m_jtdxtime = jtdxtime;
  if (wrapped_) wrapped_->start (0,m_jtdxtime);

  url_.setUrl("ws://" + server_); //server_
  if (url_.host() == "") url_.setHost("localhost");
  if (url_.port() == -1) url_.setPort(40001);

  if (!commander_)
    {
      commander_ = new QWebSocket {}; // QObject takes ownership
      connect(commander_,SIGNAL(connected()),this,SLOT(onConnected()));
      connect(commander_,SIGNAL(disconnected()),this,SLOT(onDisconnected()));
      connect(commander_,SIGNAL(binaryMessageReceived(QByteArray)),this,SLOT(onBinaryReceived(QByteArray)));
      connect(commander_,SIGNAL(textMessageReceived(QString)),this,SLOT(onMessageReceived(QString)));
      connect(commander_,SIGNAL(error(QAbstractSocket::SocketError)),this,SLOT(onError(QAbstractSocket::SocketError)));
    }
  if (!tci_loop1_) {
    tci_loop1_ = new QEventLoop  {this};
  }
  if (!tci_timer1_) {
    tci_timer1_ = new QTimer {this};
    tci_timer1_ -> setSingleShot(true);
    connect( tci_timer1_, &QTimer::timeout, tci_loop1_, &QEventLoop::quit);
    connect( this, &TCITransceiver::tci_done1, tci_loop1_, &QEventLoop::quit);
  }
  if (!tci_loop2_) {
    tci_loop2_ = new QEventLoop  {this};
  }
  if (!tci_timer2_) {
    tci_timer2_ = new QTimer {this};
    tci_timer2_ -> setSingleShot(true);
    connect( tci_timer2_, &QTimer::timeout, tci_loop2_, &QEventLoop::quit);
    connect( this, &TCITransceiver::tci_done2, tci_loop2_, &QEventLoop::quit);
  }
  if (!tci_loop3_) {
    tci_loop3_ = new QEventLoop  {this};
  }
  if (!tci_timer3_) {
    tci_timer3_ = new QTimer {this};
    tci_timer3_ -> setSingleShot(true);
    connect( tci_timer3_, &QTimer::timeout, tci_loop3_, &QEventLoop::quit);
    connect( this, &TCITransceiver::tci_done3, tci_loop3_, &QEventLoop::quit);
  }
  tci_Ready = false;
  freq_mode = false;
  trxA = 0;
  trxB = 0;
  busy_rx_frequency_ = false;
  busy_other_frequency_ = false;
  busy_drive_ = false;
  busy_PTT_ = false;
  split_ = false;
  requested_split_ = false;
  PTT_ = false;
  requested_PTT_ = false;
  requested_mode_ = "";
  mode_ = "";
  requested_rx_frequency_ = "";
  rx_frequency_ = "";
  requested_other_frequency_ = "";
  other_frequency_ = "";
  level_ = -77;
  power_ = 0;
  m_bufferPos = 0;
  m_downSampleFactor =4;
  m_ns = 999;
  audio_ = false;
  requested_stream_audio_ = false;
  stream_audio_ = false;
  _power_ = false;
//  printf ("%s(%0.1f) TCI open %s rig_power:%d rig_power_off:%d do_snr:%d do_pwr:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),url_.toString().toStdString().c_str(),rig_power_,rig_power_off_,do_snr_,do_pwr_);
  commander_->open (url_);
  mysleep1 (1500);
  if (tci_Ready) {
    if (!_power_) {
      if (rig_power_) {
        rig_power(true);
        mysleep1(500);
        if(!_power_) throw error {tr ("TCI SDR could not be switched on")};
      } else throw error {tr ("TCI SDR is not switched on")};
    }
    if (do_snr_) {
        const QString cmd = CmdSmeter + SmDP + "0" + SmCM + "0" +  SmTZ;
        sendTextMessage(cmd);
    }
    if (!stream_audio_) {
        stream_audio (true);
        mysleep1(500);
        if (!stream_audio_) throw error {tr ("TCI Audio could not be switched on")};
    }
    do_poll ();

    TRACE_CAT ("TCITransceiver", "started");
//    printf("%s(%0.1f) TCI Transceiver started\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
    return 0;
  } else throw error {tr ("TCI could not be opened")};
}

void TCITransceiver::do_stop ()
{
//  printf ("TCI close\n");
  if (stream_audio_ && tci_Ready) {stream_audio (false); mysleep1(500);// printf ("TCI audio closed\n");
}
  if (_power_ && rig_power_off_ && tci_Ready) {rig_power(false); mysleep1(500);// printf ("TCI power down\n");
}
  tci_Ready = false;
  if (commander_)
    {
      commander_->close(QWebSocketProtocol::CloseCodeNormal,"end");
      delete commander_, commander_ = nullptr;
    }
  if (tci_timer1_)
    {
      if (tci_timer1_->isActive()) tci_timer1_->stop();
      delete tci_timer1_, tci_timer1_ = nullptr;
    }
  if (tci_loop1_)
    {
      tci_loop1_->quit();
      delete tci_loop1_, tci_loop1_ = nullptr;
    }
  if (tci_timer2_)
    {
      if (tci_timer2_->isActive()) tci_timer2_->stop();
      delete tci_timer2_, tci_timer2_ = nullptr;
    }
  if (tci_loop2_)
    {
      tci_loop2_->quit();
      delete tci_loop2_, tci_loop2_ = nullptr;
    }
  if (tci_timer3_)
    {
      if (tci_timer3_->isActive()) tci_timer3_->stop();
      delete tci_timer3_, tci_timer3_ = nullptr;
    }
  if (tci_loop3_)
    {
      tci_loop3_->quit();
      delete tci_loop3_, tci_loop3_ = nullptr;
    }

  if (wrapped_) wrapped_->stop ();
  TRACE_CAT ("TCITransceiver", "stopped");
//  printf ("TCI closed\n");
}

void TCITransceiver::onMessageReceived(const QString &str)
{
//qDebug() << "From WEB" << str;
    QStringList cmd_list = str.split(";", SkipEmptyParts);
    for (QString cmds : cmd_list){
        QStringList cmd = cmds.split(":", SkipEmptyParts);
        QStringList args = cmd.last().split(",", SkipEmptyParts);
        Tci_Cmd idCmd = mapCmd_[cmd.first()];
//        if (idCmd != Cmd_Power && idCmd != Cmd_SWR && idCmd != Cmd_Smeter && idCmd != Cmd_AppFocus) { printf ("%s(%0.1f) TCI message received:|%s| ",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),str.toStdString().c_str()); printf("idCmd : %d args : %s\n",idCmd,args.join("|").toStdString().c_str());}
//qDebug() << cmds << idCmd;
        if (idCmd <=0)
            continue;
        switch (idCmd) {
        case Cmd_Smeter:
          if(args.at(0)=="0" && args.at(1) == "0") level_ = args.at(2).toInt() + 73;
          break;	
        case Cmd_SWR:
//          printf("%s(%0.1f) Cmd_SWR : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
          swr_ = 10 * args.at(0).split(".")[0].toInt() + args.at(0).split(".")[1].toInt();
          break;	
        case Cmd_Power:
          power_ = 10 * args.at(0).split(".")[0].toInt() + args.at(0).split(".")[1].toInt();
//          printf("%s(%0.1f) Cmd_Power : %s %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str(),power_);
          break;	
        case Cmd_VFO:
//            printf("%s(%0.1f) Cmd_VFO : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="0" && args.at(1) == "0") {
              rx_frequency_ = args.at(2);
              if (requested_rx_frequency_.isEmpty()) {requested_rx_frequency_ = rx_frequency_; }
              if (tci_Ready) {
                if (requested_mode_ == mode_) tci_done1();
              }
            }
            else if (args.at(0)=="0" && args.at(1) == "1") {
              if (requested_other_frequency_.isEmpty()) requested_other_frequency_ = other_frequency_;
              other_frequency_ = args.at(2);
              if (tci_Ready) {
                if (split_ == requested_split_) tci_done2();

              }
            }
            break;
        case Cmd_Mode:
//            printf("%s(%0.1f) Cmd_Mode : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="0") {
              mode_ = args.at(1);
              if (requested_mode_.isEmpty()) requested_mode_ = mode_;
              if (requested_mode_ != mode_ && !freq_mode) {
                sendTextMessage(mode_to_command(requested_mode_));
                freq_mode = true;
              }
              else if (tci_Ready && requested_rx_frequency_ == rx_frequency_) tci_done1();
            }
            break;
        case Cmd_SplitEnable:
//            printf("%s(%0.1f) Cmd_SplitEnable : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="0") {
              if (args.at(1) == "false") split_ = false;
              else if (args.at(1) == "true") split_ = true;
              if (tci_Ready &&  requested_other_frequency_ == other_frequency_) tci_done2();
            }
            break;
        case Cmd_Drive:
//            printf("%s(%0.1f) Cmd_Drive : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            drive_ = args.at(0);
            busy_drive_ = false;
            break;
        case Cmd_Trx:
//            printf("%s(%0.1f) Cmd_Trx : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="0") {
              if (args.at(1) == "false") PTT_ = false;
              else if (args.at(1) == "true") PTT_ = true;
              if (tci_Ready && requested_PTT_ == PTT_) tci_done3();
            }
            break;
        case Cmd_AudioStart:
//          printf("%s(%0.1f) Cmd_AudioStart : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="0") {
              stream_audio_ = true;
              tci_done1();
            }
          break;	
        case Cmd_AudioStop:
//          printf("%s(%0.1f) CmdAudioStop : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="0") {
              stream_audio_ = false;
              tci_done1();
            }
          break;	
        case Cmd_Start:
//          printf("%s(%0.1f) CmdStart : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
          _power_ = true;
          tci_done1();
          break;	
        case Cmd_Stop:
//          printf("%s(%0.1f) CmdStop : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
          _power_ = false;
          tci_done1();
          break;	
        case Cmd_Tune:
//          printf("%s(%0.1f) CmdTune : %s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),args.join("|").toStdString().c_str());
            if(args.at(0)=="1" && !tci_Ready) {
              tci_Ready = true;
              tci_done1();
            }
          break;	
        
        default:
            break;
        }
    }

}

void TCITransceiver::sendTextMessage(const QString &message)
{
    commander_->sendTextMessage(message);
}


void TCITransceiver::onBinaryReceived(const QByteArray &data)
{
/*    if (++cntIQ % 50 == 0){
        bIQ = !bIQ;
        nIqBytes+=data.size();
        printf("receiveIQ\n");
        emit receiveIQ(bIQ,nIqBytes);
        nIqBytes = 0;
    } else {
        nIqBytes+=data.size();
    } */
    Data_Stream *pStream = (Data_Stream*)(data.data());
    if (pStream->type != last_type) {
//        printf ("%s(%0.1f) binary resceived type=%d last_type=%d %d samplerate %d stream_size %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),pStream->type,last_type,data.size(),pStream->sampleRate,pStream->length);
        last_type = pStream->type;
    }
    if (pStream->type == Iq_Stream){
        bool tx = false;
        if (pStream->receiver == 0){
            tx = trxA == 0;
            trxA = 1;

        }
        if (pStream->receiver == 1) {
            tx = trxB == 0;
            trxB = 1;
        }
//        printf("sendIqData\n");
        emit sendIqData(pStream->receiver,pStream->length,pStream->data,tx);
qDebug() << "IQ" << data.size() << pStream->length;
    } else if (pStream->type == RxAudioStream && audio_){
//        printf("%s(%0.1f) writeAudioData\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
        writeAudioData(pStream->data,pStream->length);
qDebug() << "Audio" << data.size() << pStream->length;
    } else if (pStream->type == TxChrono &&  pStream->receiver == 0){
        int ssize = AudioHeaderSize+pStream->length*sizeof(float)*2;
//        printf("%s(%0.1f) TxChrono ",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
        quint16 tehtud;
        if (m_tx1.size() != ssize) m_tx1.resize(ssize);
        Data_Stream * pOStream1 = (Data_Stream*)(m_tx1.data());
        pOStream1->receiver = pStream->receiver;
        pOStream1->sampleRate = pStream->sampleRate; 
        pOStream1->format = pStream->format;
        pOStream1->codec = 0;
        pOStream1->crc = 0;
        pOStream1->length = pStream->length;
        pOStream1->type = TxAudioStream;
        for (size_t i = 0; i < pStream->length; i++) pOStream1->data[i] = 0;

//        printf("%s(%0.1f) txAudioChrono %d %d %d",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),ssize,pStream->length,pStream->sampleRate);
        tehtud = readAudioData(pOStream1->data,pOStream1->length);
//        printf(" %s(%0.1f) tehtud%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),tehtud);
        if (tehtud && tehtud != pOStream1->length) {
          quint32 valmis = tehtud;
//          printf("%s(%0.1f) Audio build 1 mismatch requested %d done %d",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),pOStream1->length,tehtud);
          tehtud = readAudioData(pOStream1->data + valmis,pOStream1->length - valmis);
//          printf("got %d\n",tehtud);
//          if (tehtud && tehtud != pOStream1->length - valmis) {
//            valmis += tehtud;
//            for (size_t i = 0; i < pOStream1->length - valmis; i++) pOStream1->data[i+valmis] = 0;
//          }
//          else if (tehtud == 0)  for (size_t i = 0; i < pOStream1->length - valmis; i++) pOStream1->data[i+valmis] = 0;
//        }
//        else if (tehtud == 0) {
//          for (size_t i = 0; i < pStream->length; i++) pOStream1->data[i] = 0;
        }
        if (commander_->sendBinaryMessage(m_tx1) != m_tx1.size()) printf("%s(%0.1f) Sent 1 loaded failed\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
    }
}

void TCITransceiver::txAudioData(quint32 len, float * data)
{
    QByteArray tx;
//    printf("%s(%0.1f) txAudioData %d %ld\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),len,(AudioHeaderSize+len*sizeof(float)*2));
    tx.resize(AudioHeaderSize+len*sizeof(float)*2);
//    printf("%s(%0.1f) txAudioData %d %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),len,tx.size());
    Data_Stream * pStream = (Data_Stream*)(tx.data());
    pStream->receiver = 0;
    pStream->sampleRate = audioSampleRate;
    pStream->format = 3;
    pStream->codec = 0;
    pStream->crc = 0;
    pStream->length = len;
    pStream->type = TxAudioStream;
    memcpy(pStream->data,data,len*sizeof(float)*2);
    commander_->sendBinaryMessage(tx);
}

quint32 TCITransceiver::writeAudioData (float * data, qint32 maxSize)
{
  static unsigned mstr0=999999;
  qint64 ms0 = m_jtdxtime->currentMSecsSinceEpoch2() % 86400000; //m_jtdxtime -> currentMSecsSinceEpoch2() % 86400000;
  unsigned mstr = ms0 % int(1000.0*m_period); // ms into the nominal Tx start time
  if(mstr < mstr0) {              //When mstr has wrapped around to 0, restart the buffer
    dec_data.params.kin = 0;
    m_bufferPos = 0;
//    printf("%s(%0.1f) reset buffer mstr:%d mstr0:%d maxSize:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),mstr,mstr0,maxSize);
  }
  mstr0=mstr;

  // no torn frames
  Q_ASSERT (!(maxSize % static_cast<qint32> (bytesPerFrame)));
  // these are in terms of input frames (not down sampled)
  size_t framesAcceptable ((sizeof (dec_data.d2) /
                            sizeof (dec_data.d2[0]) - dec_data.params.kin) * m_downSampleFactor);
  size_t framesAccepted (qMin (static_cast<size_t> (maxSize /
                                                    bytesPerFrame), framesAcceptable));

  if (framesAccepted < static_cast<size_t> (maxSize / bytesPerFrame)) {
    qDebug () << "dropped " << maxSize / bytesPerFrame - framesAccepted
                << " frames of data on the floor!"
                << dec_data.params.kin << mstr;
    printf("%s(%0.1f) dropped %ld frames of data %d %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),maxSize / bytesPerFrame - framesAccepted,dec_data.params.kin,mstr);
    }

    for (unsigned remaining = framesAccepted; remaining; ) {
      size_t numFramesProcessed (qMin (m_samplesPerFFT *
                                       m_downSampleFactor - m_bufferPos, remaining));

      if(m_downSampleFactor > 1) {
//  printf ("%s(%0.1f) writeAudioData maxs %d bytesPerFrame %ld Accepted %ld remaining %d Processed %ld Bufferpos %d kin %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),
//  m_jtdxtime->GetOffset(),maxSize,bytesPerFrame,framesAccepted,remaining,numFramesProcessed,m_bufferPos,dec_data.params.kin);
        store (&data[(framesAccepted - remaining) * bytesPerFrame],
               numFramesProcessed, &m_buffer[m_bufferPos]);
        m_bufferPos += numFramesProcessed;

        if(m_bufferPos==m_samplesPerFFT*m_downSampleFactor) {
          qint32 framesToProcess (m_samplesPerFFT * m_downSampleFactor);
          qint32 framesAfterDownSample (m_samplesPerFFT);
          if(m_downSampleFactor > 1 && dec_data.params.kin>=0 &&
             dec_data.params.kin < (NTMAX*12000 - framesAfterDownSample)) {
            fil4_(&m_buffer[0], &framesToProcess, &dec_data.d2[dec_data.params.kin],
                  &framesAfterDownSample, &dec_data.dd2[dec_data.params.kin]);
            dec_data.params.kin += framesAfterDownSample;
          } else {
            // qDebug() << "framesToProcess     = " << framesToProcess;
            // qDebug() << "dec_data.params.kin = " << dec_data.params.kin;
            // qDebug() << "secondInPeriod      = " << secondInPeriod();
            // qDebug() << "framesAfterDownSample" << framesAfterDownSample;
          }
//    printf("%s(%0.1f) frameswritten %d downSampleFactor %d samplesPerFFT %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),dec_data.params.kin,m_downSampleFactor,m_samplesPerFFT);
          Q_EMIT tciframeswritten (dec_data.params.kin);
//    printf("%s(%0.1f) frameswritten done\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
          m_bufferPos = 0;
        }

      } else {
//         printf ("%s(%0.1f) writeAudioData2 maxs %d bytesPerFrame %ld Accepted %ld remaining %d Processed %ld Bufferpos %d kin %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),maxSize,bytesPerFrame,framesAccepted,remaining,numFramesProcessed,m_bufferPos,dec_data.params.kin);
         store (&data[(framesAccepted - remaining) * bytesPerFrame],
               numFramesProcessed, &dec_data.d2[dec_data.params.kin]);
        m_bufferPos += numFramesProcessed;
        dec_data.params.kin += numFramesProcessed;
        if (m_bufferPos == static_cast<unsigned> (m_samplesPerFFT)) {
//          printf("%s(%0.1f) frameswritten2 %d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),dec_data.params.kin);
          Q_EMIT tciframeswritten (dec_data.params.kin);
//          printf("%s(%0.1f) frameswritten2 done\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
          m_bufferPos = 0;
        }
      }
      remaining -= numFramesProcessed;
    }



  return maxSize;    // we drop any data past the end of the buffer on
  // the floor until the next period starts
}

  void TCITransceiver::rig_power (bool on)
{
  TRACE_CAT ("TCITransceiver", on << state ());
//  printf ("%s(%0.1f) TCI rig_power:%d _power_:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),on,_power_);
  if (on != _power_) {
    if (on) {
      const QString cmd = CmdStart + SmTZ;
      sendTextMessage(cmd);
    } else {
      const QString cmd = CmdStop + SmTZ;
      sendTextMessage(cmd);
    }
  } 

}

  void TCITransceiver::stream_audio (bool on)
{
  TRACE_CAT ("TCITransceiver", on << state ());
//  printf ("%s(%0.1f) TCI stream_audio:%d stream_audio_:%d requested_stream_audio_:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),on,stream_audio_,requested_stream_audio_);
  if (on != stream_audio_) {
    requested_stream_audio_ = on;
    if (on) {
      const QString cmd = CmdAudioStart + SmDP + "0" + SmTZ;
      sendTextMessage(cmd);
    } else {
      const QString cmd = CmdAudioStop + SmDP + "0" + SmTZ;
      sendTextMessage(cmd);
    }
  } 

}

  void TCITransceiver::do_audio (bool on)
{
  TRACE_CAT ("TCITransceiver", on << state ());
//  printf ("%s(%0.1f) TCI do_audio:%d audio_:%d state:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),on,audio_,state().audio());
  if (on) {
    dec_data.params.kin = 0;
    m_bufferPos = 0;
  }
  audio_ = on;
}

  void TCITransceiver::do_period (double period)
{
  TRACE_CAT ("TCITransceiver", period << state ());
//  printf ("%s(%0.1f) TCI do_period:%0.1f m_period_:%0.1f state:%0.1f\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),period,m_period,state().period());
  m_period = period;
}

  void TCITransceiver::do_txvolume (qreal volume)
{
  TRACE_CAT ("TCITransceiver", period << state ());
  QString drive = QString::number(round(100 - volume * 2.2222222));
//  printf ("%s(%0.1f) TCI do_txvolume:%0.1f state:%0.1f drive:%s drive_:%s drive_busy:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),volume,state().volume(),drive.toStdString().c_str(),drive_.toStdString().c_str(),busy_drive_);
  if (busy_drive_ || !tci_Ready || requested_drive_ == drive || drive_ == drive) return;
  else  busy_drive_ = true;
  requested_drive_ = drive;
  const QString cmd = CmdDrive + SmDP + drive + SmTZ;
  sendTextMessage(cmd);
}

  void TCITransceiver::do_blocksize (qint32 blocksize)
{
  TRACE_CAT ("TCITransceiver", blocksize << state ());
//  printf ("%s(%0.1f) TCI do_blocksize:%d m_samplesPerFFT:%d state:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),blocksize,m_samplesPerFFT,state().blocksize());
  m_samplesPerFFT = blocksize;
}

  void TCITransceiver::do_ptt (bool on)
{
  TRACE_CAT ("TCITransceiver", on << state ());
//  printf ("%s(%0.1f) TCI do_ptt:%d PTT_:%d requested_PTT_:%d state:%d use_for_ptt:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),on,PTT_,requested_PTT_,state().ptt(),use_for_ptt_);
  if (use_for_ptt_)
    {
      if (on != PTT_) {
        if (busy_PTT_ || !tci_Ready) return;
        else busy_PTT_ = true;
        requested_PTT_ = on;
        const QString cmd = CmdTrx + SmDP + "0" + SmCM + (on ? "true" : "false") + SmTZ;
        sendTextMessage(cmd);
        mysleep3(1000);
        busy_PTT_ = false;
        if (requested_PTT_ == PTT_) update_PTT(PTT_);
        else {
//          printf ("%s(%0.1f) TCI failed set ptt %d->%d}n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),PTT_,requested_PTT_);
          throw error {tr ("TCI failed to set ptt")};
        }
      } else update_PTT(on); 
    }
  else
    {
          TRACE_CAT ("TCITransceiver", "TCI should use PTT via CAT");
          throw error {tr ("TCI should use PTT via CAT")};
    }
}

void TCITransceiver::do_frequency (Frequency f, MODE m, bool no_ignore)
{
  TRACE_CAT ("TCITransceiver", f << state ());
  auto f_string = frequency_to_string (f);
//  printf ("%s(%0.1f) TCI do_frequency:%s current_frequency:%s mode:%s current_mode:%s no_ignore:%d busy:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),f_string.toStdString().c_str(),rx_frequency_.toStdString().c_str(),map_mode(m).toStdString().c_str(),requested_mode_.toStdString().c_str(),no_ignore,busy_rx_frequency_);
  if  (tci_Ready && busy_rx_frequency_ && no_ignore) printf ("%s(%0.1f) TCI do_frequency critical no_ignore set vfo will be missed\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
  if (busy_rx_frequency_ || !tci_Ready) return;
  else  busy_rx_frequency_ = true;
  requested_mode_ = map_mode (m);
  if (rx_frequency_ != f_string && requested_rx_frequency_ != f_string) {
    requested_rx_frequency_ = f_string;
    const QString cmd = CmdVFO + SmDP + "0" + SmCM + "0" + SmCM + requested_rx_frequency_ + SmTZ;
    if (mode_ != requested_mode_ && !requested_mode_.isEmpty()) {
//      printf ("%s(%0.1f) setting both freq and mode\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
      freq_mode = true;
      sendTextMessage(cmd + mode_to_command(requested_mode_));
    } else {
      freq_mode = false;
      sendTextMessage(cmd);
    }
    mysleep1(1000);
    if (requested_rx_frequency_ == rx_frequency_) update_rx_frequency (f);
    else {
//      printf ("%s(%0.1f) TCI failed set rxfreq:%s->%s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),rx_frequency_.toStdString().c_str(),requested_rx_frequency_.toStdString().c_str());
      throw error {tr ("TCI failed set rxfreq")};
    }
    if (requested_mode_.isEmpty() || requested_mode_ == mode_) update_mode (m);
    else {
//      printf ("%s(%0.1f) TCI failed set mode %s->%s",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),mode_.toStdString().c_str(),requested_mode_.toStdString().c_str());
      throw error {tr ("TCI failed set mode")};
    }
  } else if (!requested_mode_.isEmpty() && requested_mode_ != mode_) {
    sendTextMessage(mode_to_command(requested_mode_));
    mysleep1(1000);
    if (requested_mode_.isEmpty() || requested_mode_ == mode_) update_mode (m);
    else {
//      printf ("%s(%0.1f) TCI failed set mode %s->%s",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),mode_.toStdString().c_str(),requested_mode_.toStdString().c_str());
      throw error {tr ("TCI failed set mode")};
    }
  } 
  busy_rx_frequency_ = false;
}

void TCITransceiver::do_tx_frequency (Frequency tx, MODE mode, bool no_ignore)
{
  TRACE_CAT ("TCITransceiver", tx << state ());
  auto f_string = frequency_to_string (tx);
//  printf ("%s(%0.1f) TCI do_tx_frequency:%s current_frequency:%s mode:%s no_ignore:%d busy:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),f_string.toStdString().c_str(),other_frequency_.toStdString().c_str(),map_mode(mode).toStdString().c_str(),no_ignore,busy_other_frequency_);
  if  (tci_Ready && busy_other_frequency_ && no_ignore) printf ("%s(%0.1f) TCI do_txfrequency critical no_ignore set tx vfo will be missed\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset());
  if (busy_other_frequency_ || !tci_Ready) return;
  else  busy_other_frequency_ = true;
  requested_mode_ = map_mode (mode);
  if (tx)
    {
      if (other_frequency_ != f_string && requested_other_frequency_ != f_string) {
        requested_other_frequency_ = f_string;
        const QString cmd = CmdVFO + SmDP + "0" + SmCM + "1" + SmCM + f_string + SmTZ;
        requested_split_ = true;
        if (requested_split_ != split_) {
          const QString cmd2 = CmdSplitEnable + SmDP + "0" + SmCM + "true" + SmTZ;
          sendTextMessage(cmd + cmd2);
        } else sendTextMessage(cmd);
        mysleep2(1000);
        if (requested_other_frequency_ == other_frequency_) update_other_frequency (tx);
        else {
//          printf ("%s(%0.1f) TCI failed set txfreq:%s->%s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),other_frequency_.toStdString().c_str(),requested_other_frequency_.toStdString().c_str());
          throw error {tr ("TCI failed set txfreq")};
        }
        if (requested_split_ == split_) update_split (split_);
        else {
//          printf("%s(%0.1f) TCI failed set split %d->%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),split_,requested_split_);
          throw error {tr ("TCI failed set split")};
        }
      }
    }
  else
    {
      requested_split_ = false;
      if (requested_split_ != split_) {
         const QString cmd = CmdSplitEnable + SmDP + "0" + SmCM + "false" + SmTZ;
         sendTextMessage(cmd);
         mysleep2(1000);
        if (requested_split_ == split_) update_split (split_);
        else {
//          printf("%s(%0.1f) TCI failed set split %d->%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),split_,requested_split_);
          throw error {tr ("TCI failed set split")};
        }
      }
    }
  busy_other_frequency_ = false;
}

void TCITransceiver::do_mode (MODE m)
{
  TRACE_CAT ("TCITransceiver", m << state ());
  auto m_string = map_mode (m);
//  printf ("%s(%0.1f) TCI do_mode:%s->%s\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),mode_.toStdString().c_str(),m_string.toStdString().c_str());
  if (requested_mode_ != m_string) requested_mode_ = m_string;
  if (mode_ != m_string) {
    sendTextMessage(mode_to_command(m_string));
    mysleep1(1000);
    if (requested_mode_ == mode_) update_mode (m);
    else {
//      printf ("%s(%0.1f) TCI failed set mode %s->%s",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),mode_.toStdString().c_str(),requested_mode_.toStdString().c_str());
      throw error {tr ("TCI failed set mode")};
    }
  }
}

void TCITransceiver::do_poll ()
{
//#if WSJT_TRACE_CAT && WSJT_TRACE_CAT_POLLS
//  bool quiet {false};
//#else
//  bool quiet {true};
//#endif
//  printf("%s(%0.1f) TCI do_poll split:%d ptt:%d rx_busy:%d tx_busy:%d level:%d power:%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),state ().split (),state (). ptt (),busy_rx_frequency_,busy_other_frequency_,level_,power_);
  update_rx_frequency (string_to_frequency (rx_frequency_));
  if (state ().split ()) update_other_frequency (string_to_frequency (other_frequency_));
  update_split (split_);
  update_mode (get_mode());
  if (do_pwr_) update_power (power_ * 100);
  if (do_snr_) {
      update_level (level_);
      const QString cmd = CmdSmeter + SmDP + "0" + SmCM + "0" +  SmTZ;
      sendTextMessage(cmd);
  }
}

auto TCITransceiver::get_mode (bool requested) -> MODE
{
  MODE m {UNK};
  if (requested) {
    if ("am" == requested_mode_)
      {
        m = AM;
      }
    else if ("cw" == requested_mode_)
      {
        m = CW;
      }
    else if ("wfm" == requested_mode_)
      {
        m = FM;
      }
    else if ("nfm" == requested_mode_)
      {
        m = DIG_FM;
      }
    else if ("lsb" == requested_mode_)
      {
        m = LSB;
      }
    else if ("usb" == requested_mode_)
      {
        m = USB;
      }
    else if ("digl" == requested_mode_)
      {
        m = DIG_L;
      }
    else if ("digu" == requested_mode_)
      {
        m = DIG_U;
      }
    else
      {
        m = USB;
      }
  } else {
    if ("am" == mode_)
      {
        m = AM;
      }
    else if ("cw" == mode_)
      {
        m = CW;
      }
    else if ("wfm" == mode_)
      {
        m = FM;
      }
    else if ("nfm" == mode_)
      {
        m = DIG_FM;
      }
    else if ("lsb" == mode_)
      {
        m = LSB;
      }
    else if ("usb" == mode_)
      {
        m = USB;
      }
    else if ("digl" == mode_)
      {
        m = DIG_L;
      }
    else if ("digu" == mode_)
      {
        m = DIG_U;
      }
    else
      {
        m = USB;
      }
  }
  return m;
}

QString TCITransceiver::mode_to_command (QString m_string) const
{
    const QString cmd = CmdMode + SmDP + "0" + SmCM + m_string + SmTZ;
    return cmd;
}

QString TCITransceiver::frequency_to_string (Frequency f) const
{
  // number is localized and in kHz, avoid floating point translation
  // errors by adding a small number (0.1Hz)
  auto f_string = QString {"%L2"}.arg (f);
//  printf ("frequency_to_string1 %s\n",f_string.toStdString().c_str());
  f_string = f_string.simplified().remove(' ');
//  printf ("frequency_to_string2 %s\n",f_string.toStdString().c_str());
  f_string = f_string.replace(",","");
//  printf ("frequency_to_string3 %s\n",f_string.toStdString().c_str());
  return f_string;
  
}

auto TCITransceiver::string_to_frequency (QString s) const -> Frequency
{
  // temporary hack because Commander is returning invalid UTF-8 bytes
  s.replace (QChar {QChar::ReplacementCharacter}, locale_.groupSeparator ());

  bool ok;

  auto f = QLocale::c ().toDouble (s, &ok); // temporary fix

  if (!ok)
    {
      throw error {tr ("TCI sent an unrecognized frequency")};
    }
  return f;
}

void TCITransceiver::mysleep1 (int ms)
{
//  tci_timer1->setSingleShot(true);
  tci_timer1_->start(ms);
  tci_loop1_->exec();
  if (tci_timer1_->isActive() && tci_Ready) tci_timer1_->stop();
}
void TCITransceiver::mysleep2 (int ms)
{
//  tci_timer2->setSingleShot(true);
  tci_timer2_->start(ms);
  tci_loop2_->exec();
  if (tci_timer2_->isActive()) tci_timer2_->stop();
}
void TCITransceiver::mysleep3 (int ms)
{
//  tci_timer3->setSingleShot(true);
  tci_timer3_->start(ms);
  tci_loop3_->exec();
  if (tci_timer3_->isActive()) tci_timer3_->stop();
}
// Modulator part

void TCITransceiver::do_modulator_start (unsigned symbolsLength, double framesPerSymbol,
                       double frequency, double toneSpacing, bool synchronize, double dBSNR, double TRperiod)
{
//  QThread::currentThread()->setPriority(QThread::HighPriority);
//  Q_ASSERT (stream);

// Time according to this computer which becomes our base time
  qint64 ms0 = m_jtdxtime->currentMSecsSinceEpoch2() % 86400000;
//  qDebug() << "ModStart" << QDateTime::currentDateTimeUtc().toString("hh:mm:ss.sss");
  unsigned mstr = ms0 % int(1000.0*m_period); // ms into the nominal Tx start time
  if (m_state != Idle) {
//    stop ();
    throw error {tr ("TCI modulator not Idle")};
  }
  m_quickClose = false;
  m_symbolsLength = symbolsLength;
  m_isym0 = std::numeric_limits<unsigned>::max (); // big number
  m_frequency0 = 0.;
  m_phi = 0.;
  m_addNoise = dBSNR < 0.;
  m_nsps = framesPerSymbol;
  m_trfrequency = frequency;
  m_amp = std::numeric_limits<qint16>::max ();
  m_toneSpacing = toneSpacing;
  m_TRperiod=TRperiod;
  unsigned delay_ms=1000;
  if(m_nsps==1920) delay_ms=500;   //FT8
  else if(m_nsps==576) {
    delay_ms=500;   //FT4
  }
  // noise generator parameters
  if (m_addNoise) {
    m_snr = qPow (10.0, 0.05 * (dBSNR - 6.0));
    m_fac = 3000.0;
    if (m_snr > 1.0) m_fac = 3000.0 / m_snr;
  }

  // round up to an exact portion of a second that allows for startup delays
  //m_ic = (mstr / delay_ms) * audioSampleRate * delay_ms / 1000;
  auto mstr2 = mstr - delay_ms;
  if (mstr <= delay_ms) {
    m_ic = 0;
  } else {
    m_ic = mstr2 * (audioSampleRate / 1000);
  }
  m_silentFrames = 0;
  // calculate number of silent frames to send
  if (m_ic == 0 && synchronize && !m_tuning)	{
    m_silentFrames = audioSampleRate / (1000 / delay_ms) - (mstr * (audioSampleRate / 1000));
  }
#if JTDX_DEBUG_TO_FILE
  FILE * pFile = fopen (debug_file_.c_str(),"a");  
//  fprintf (pFile,"delay_ms=%d audioSampleRate=%d mstr=%d mstr2 = %d m_ic=%d m_silentFrames=%lld \n",delay_ms,audioSampleRate,mstr,mstr2,m_ic,m_silentFrames);
  fclose (pFile);
#endif
  m_state = (synchronize && m_silentFrames) ?
                        Synchronizing : Active;
//  printf ("%s(%0.1f) TCI modulator startdelay_ms=%d ASR=%d mstr=%d mstr2=%d m_ic=%d s_Frames=%lld State=%d\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),delay_ms,audioSampleRate,mstr,mstr2,m_ic,m_silentFrames,m_state);
  Q_EMIT tci_mod_active(m_state != Idle);
}

void TCITransceiver::do_tune (bool newState)
{
  m_tuning = newState;
  if (!m_tuning) do_modulator_stop (true);
}

void TCITransceiver::do_modulator_stop (bool quick)
{
  m_quickClose = quick;
  if(m_state != Idle) {
  m_state = Idle;
  Q_EMIT tci_mod_active(m_state != Idle);
  }
  tx_audio_ = false;
}

quint16 TCITransceiver::readAudioData (float * data, qint32 maxSize)
{
  double toneFrequency=1500.0;
  if(m_nsps==6) {
    toneFrequency=1000.0;
    m_trfrequency=1000.0;
    m_frequency0=1000.0;
  }
  if(maxSize==0) return 0;

  qint64 numFrames (maxSize/bytesPerFrame);
  float * samples (reinterpret_cast<float *> (data));
  float * end (samples + numFrames * bytesPerFrame);
//  printf("%s(%0.1f) readAudioData %d %p %p\n",m_jtdxtime->currentDateTimeUtc2().toString("hh:mm:ss.zzz").toStdString().c_str(),m_jtdxtime->GetOffset(),maxSize,samples,end);
  qint64 framesGenerated (0);

  switch (m_state)
    {
    case Synchronizing:
      {
        if (m_silentFrames)	{  // send silence up to first second
          framesGenerated = qMin (m_silentFrames, numFrames);
          for ( ; samples != end; samples = load (0, samples)) { // silence
          }
          m_silentFrames -= framesGenerated;
          return framesGenerated * bytesPerFrame;
        }
        m_state = Active;
        Q_EMIT tci_mod_active(m_state != Idle);
        m_cwLevel = false;
        m_ramp = 0;		// prepare for CW wave shaping
      }
      // fall through

    case Active:
      {
        unsigned int isym=0;
        qint16 sample=0;
        if(!m_tuning) isym=m_ic/(4.0*m_nsps);            // Actual fsample=48000
		bool slowCwId=((isym >= m_symbolsLength) && (icw[0] > 0));
        m_nspd=2560;                 // 22.5 WPM

        if(m_TRperiod > 16.0 && slowCwId) {     // Transmit CW ID?
          m_dphi = m_twoPi*m_trfrequency/audioSampleRate;
          unsigned ic0 = m_symbolsLength * 4 * m_nsps;
          unsigned j(0);

          while (samples != end) {
            j = (m_ic - ic0)/m_nspd + 1; // symbol of this sample
            bool level {bool (icw[j])};
            m_phi += m_dphi;
            if (m_phi > m_twoPi) m_phi -= m_twoPi;
            sample=0;
            float amp=32767.0;
            float x=0.0;
            if(m_ramp!=0) {
              x=qSin(float(m_phi));
              if(SOFT_KEYING) {
                amp=qAbs(qint32(m_ramp));
                if(amp>32767.0) amp=32767.0;
              }
              sample=round(amp*x);
            }
            if (int (j) <= icw[0] && j < NUM_CW_SYMBOLS) { // stopu condition
              samples = load (postProcessSample (sample), samples);
              ++framesGenerated;
              ++m_ic;
            } else {
              m_state = Idle;
              Q_EMIT tci_mod_active(m_state != Idle);
              return framesGenerated * bytesPerFrame;
            }

            // adjust ramp
            if ((m_ramp != 0 && m_ramp != std::numeric_limits<qint16>::min ()) || level != m_cwLevel) {
              // either ramp has terminated at max/min or direction has changed
              m_ramp += RAMP_INCREMENT; // ramp
            }
            m_cwLevel = level;
          }
          return framesGenerated * bytesPerFrame;
        } //End of code for CW ID

        double const baud (12000.0 / m_nsps);
        // fade out parameters (no fade out for tuning)
        unsigned int i0,i1;
        if(m_tuning) {
          i1 = i0 = 9999 * m_nsps;
        } else {
          i0=(m_symbolsLength - 0.017) * 4.0 * m_nsps;
          i1= m_symbolsLength * 4.0 * m_nsps;
        }

        sample=0;
        for (unsigned i = 0; i < numFrames && m_ic <= i1; ++i) {
//          printf("algus %d %lld %d %d",i,numFrames,m_ic,i1);
          if(m_TRperiod > 16.0 || m_tuning) {
            isym=0;
            if(!m_tuning) isym=m_ic / (4.0 * m_nsps);         //Actual fsample=48000
            if (isym != m_isym0 || m_trfrequency != m_frequency0) {
              if(itone[0]>=100) {
                m_toneFrequency0=itone[0];
              } else {
                if(m_toneSpacing==0.0) {
                  m_toneFrequency0=m_trfrequency + itone[isym]*baud;
                } else {
                  m_toneFrequency0=m_trfrequency + itone[isym]*m_toneSpacing;
                }
              }
//            qDebug() << "B" << m_ic << numFrames << isym << itone[isym] << toneFrequency0 << m_nsps;
              m_dphi = m_twoPi * m_toneFrequency0 / audioSampleRate;
              m_isym0 = isym;
              m_frequency0 = m_trfrequency;         //???
            }

            int j=m_ic/480;
            if(m_fSpread>0.0 and j!=m_j0) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            float x1=QRandomGenerator::global ()->generateDouble ();
            float x2=QRandomGenerator::global ()->generateDouble ();
#else
            float x1=(float)qrand()/RAND_MAX;
            float x2=(float)qrand()/RAND_MAX;
#endif
              toneFrequency = m_toneFrequency0 + 0.5*m_fSpread*(x1+x2-1.0);
              m_dphi = m_twoPi * toneFrequency / audioSampleRate;
              m_j0=j;
            }

            m_phi += m_dphi;
            if (m_phi > m_twoPi) m_phi -= m_twoPi;
            //ramp for first tone
            if (m_ic==0) m_amp = m_amp * 0.008144735;
            if (m_ic > 0 and  m_ic < 191) m_amp = m_amp / 0.975;
            //ramp for last tone
            if (m_ic > i0) m_amp = 0.99 * m_amp;
            if (m_ic > i1) m_amp = 0.0;
            sample=qRound(m_amp*qSin(m_phi));
          }
          //transmit from a precomputed FT8 wave[] array:
          if(!m_tuning and (m_toneSpacing < 0.0)) { m_amp=32767.0; sample=qRound(m_amp*foxcom_.wave[m_ic]); }
          samples = load (postProcessSample (sample), samples);
          ++framesGenerated; ++m_ic;
        }

//          printf("sample saved %lld %d\n",framesGenerated,m_ic);
        if (m_amp == 0.0) { // TODO G4WJS: compare double with zero might not be wise
          if (icw[0] == 0) {
            // no CW ID to send
            m_state = Idle;
            Q_EMIT tci_mod_active(m_state != Idle);
            return framesGenerated * bytesPerFrame;
          }
          m_phi = 0.0;
        }

        m_frequency0 = m_trfrequency;
        // done for this chunk - continue on next call
        while (samples != end) { // pad block with silence
          samples = load (0, samples);
          ++framesGenerated;
        }
        return framesGenerated * bytesPerFrame;
      }
      // fall through

    case Idle:
      break;
    }

  Q_ASSERT (Idle == m_state);
  return 0;
}

qint16 TCITransceiver::postProcessSample (qint16 sample) const
{
  if (m_addNoise) {  // Test frame, we'll add noise
    qint32 s = m_fac * (gran () + sample * m_snr / 32768.0);
    if (s > std::numeric_limits<qint16>::max ()) {
      s = std::numeric_limits<qint16>::max ();
    }
    if (s < std::numeric_limits<qint16>::min ()) {
      s = std::numeric_limits<qint16>::min ();
    }
    sample = s;
  }
  return sample;
}