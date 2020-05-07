#include "PollingTransceiver.hpp"

#include <exception>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QDateTime>
#include <QThread>
#include <QDir>
#include <QStandardPaths>
#include "moc_PollingTransceiver.cpp"

namespace
{
  unsigned const polls_to_stabilize {2};
}

PollingTransceiver::PollingTransceiver (int poll_interval, QObject * parent)
  : TransceiverBase {parent}
  , interval_ {poll_interval * 1000}
  , poll_timer_ {nullptr}
  , ft4_mode_ {false}
  , fast_mode_ {interval_ == 500}
  , retries_ {0}
  , debug_file_ {QDir(QStandardPaths::writableLocation (QStandardPaths::DataLocation)).absoluteFilePath ("jtdx_debug.txt").toStdString()}
{
#if JTDX_DEBUG_TO_FILE
FILE * pFile = fopen (debug_file_.c_str(),"a");
fprintf (pFile,"%s Polling Tranceiver created interval %d\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str(),poll_interval);
fclose (pFile);
#endif
}

void PollingTransceiver::start_timer ()
{
  if (interval_)
    {
      if (!poll_timer_)
        {
          poll_timer_ = new QTimer {this}; // pass ownership to
                                           // QObject which handles
                                           // destruction for us

          connect (poll_timer_, &QTimer::timeout, this,
                   &PollingTransceiver::handle_timeout);
        }
#if JTDX_DEBUG_TO_FILE
      FILE * pFile = fopen (debug_file_.c_str(),"a");
      fprintf(pFile,"%s Poll timer start interval=%d\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str(),interval_);
#endif
      poll_timer_->start (interval_);
#if JTDX_DEBUG_TO_FILE
      fprintf(pFile,"%s Poll timer started\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str());
      fclose (pFile);
#endif
    }
  else
    {
      stop_timer ();
    }
}

void PollingTransceiver::stop_timer ()
{
  if (poll_timer_)
    {
      poll_timer_->stop ();
    }
}

void PollingTransceiver::do_post_start ()
{
  auto ms = QDateTime::currentMSecsSinceEpoch() % 1000;
  int sec = QDateTime::currentDateTimeUtc().toString("ss").toInt() % 15;
#if JTDX_DEBUG_TO_FILE
  FILE * pFile = fopen (debug_file_.c_str(),"a");
  fprintf (pFile,"%s Poll start ms %lld %d\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str(),ms,sec);
  fclose (pFile);
#endif
  if (fast_mode_)
    if (ms <= 500)
      QThread::msleep (500-ms);
    else
      QThread::msleep (1000-ms);
  else if (ft4_mode_ && (sec == 5 || sec == 6 || sec == 7 || sec == 8 || sec == 9 || sec == 10 || sec == 11))
    QThread::msleep (1000-ms);
  else
    if (ms <= 500)
      QThread::msleep (500-ms);
    else
      QThread::msleep (1500-ms);

  start_timer ();
  if (!next_state_.online ())
    {
      // remember that we are expecting to go online
      next_state_.online (true);
      retries_ = polls_to_stabilize;
    }
}

void PollingTransceiver::do_post_stop ()
{
  // not much point waiting for rig to go offline since we are ceasing
  // polls
#if JTDX_DEBUG_TO_FILE
  FILE * pFile = fopen (debug_file_.c_str(),"a");
  fprintf (pFile,"%s Poll stop\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str());
  fclose (pFile);
#endif
  stop_timer ();
}

void PollingTransceiver::do_post_frequency (Frequency f, MODE m)
{
  // take care not to set the expected next mode to unknown since some
  // callers use mode == unknown to signify that they do not know the
  // mode and don't care
  if (next_state_.frequency () != f || (m != UNK && next_state_.mode () != m))
    {
      // update expected state with new frequency and set poll count
      next_state_.frequency (f);
      if (m != UNK)
        {
          next_state_.mode (m);
        }
      retries_ = polls_to_stabilize;
    }
}

void PollingTransceiver::do_post_tx_frequency (Frequency f, MODE)
{
  if (next_state_.tx_frequency () != f)
    {
      // update expected state with new TX frequency and set poll
      // count
      next_state_.tx_frequency (f);
      next_state_.split (f); // setting non-zero TX frequency means split
      retries_ = polls_to_stabilize;
    }
}

void PollingTransceiver::do_post_mode (MODE m)
{
  // we don't ever expect mode to goto to unknown
  if (m != UNK && next_state_.mode () != m)
    {
      // update expected state with new mode and set poll count
      next_state_.mode (m);
      retries_ = polls_to_stabilize;
    }
}

void PollingTransceiver::do_post_ft4_mode (bool p)
{
//  printf("PollingTransceiver do_post_ft4_mode = %d\n",p);
      // update polling style
      next_state_.ft4_mode (p);
      ft4_mode_ = p && (interval_ == 1000 || fast_mode_);
      if (interval_ == 1000 && !fast_mode_) do_post_start ();
}

void PollingTransceiver::do_post_ptt (bool p)
{
  if (next_state_.ptt () != p)
    {
      // update expected state with new PTT and set poll count
      next_state_.ptt (p);
      retries_ = polls_to_stabilize;
      retries_ = 1;             // fast feedback on PTT
    }
}

bool PollingTransceiver::do_pre_update ()
{
  // if we are holding off a change then withhold the signal
  if (retries_ && state () != next_state_)
    {
      return false;
    }
  return true;
}

void PollingTransceiver::handle_timeout ()
{
  QString message;
  bool force_signal {false};

  // we must catch all exceptions here since we are called by Qt and
  // inform our parent of the failure via the offline() message
  try
    {
      int sec = QDateTime::currentDateTimeUtc().toString("ss").toInt() % 15;
      auto ms = QDateTime::currentMSecsSinceEpoch() % 1000;
#if JTDX_DEBUG_TO_FILE
      FILE * pFile = fopen (debug_file_.c_str(),"a");
      fprintf(pFile,"%s %d Poll start retries=%d fast_mode=%d ft4_mode=%d\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str(),sec,retries_,fast_mode_,ft4_mode_);
      fclose (pFile);
#endif
      if (!ft4_mode_ && !fast_mode_) {
        if (ms < 125) {poll_timer_->stop (); QThread::msleep (500); poll_timer_->start (interval_);}
        else if (ms < 250) {poll_timer_->stop (); QThread::msleep (500 - ms); poll_timer_->start (interval_);}
        else if (ms > 875) {poll_timer_->stop (); QThread::msleep (500); poll_timer_->start (interval_);}
        else if (ms > 750) {poll_timer_->stop (); QThread::msleep (1500 - ms); poll_timer_->start (interval_);}
      } else if (!fast_mode_) {
        if ((sec == 6 || sec == 7 || sec == 8 || sec == 9 || sec == 10 || sec == 11 || sec == 12) && ms > 250 && ms < 750) {
          if (ms > 375 && ms < 625) {poll_timer_->stop (); QThread::msleep (500); poll_timer_->start (interval_);}
          else {poll_timer_->stop (); QThread::msleep (1000 - ms); poll_timer_->start (interval_);}
        } else if (sec == 13 || sec == 14 || sec == 0 || sec == 1 || sec == 2 || sec == 3 || sec == 14) {
          if (ms < 125) {poll_timer_->stop (); QThread::msleep (500); poll_timer_->start (interval_);}
          else if (ms < 250) {poll_timer_->stop (); QThread::msleep (500 - ms); poll_timer_->start (interval_);}
          else if (ms > 875) {poll_timer_->stop (); QThread::msleep (500); poll_timer_->start (interval_);}
          else if (ms > 750) {poll_timer_->stop (); QThread::msleep (1500 - ms); poll_timer_->start (interval_);}
        }
      }
      do_poll ();              // tell sub-classes to update our state
#if JTDX_DEBUG_TO_FILE
      pFile = fopen (debug_file_.c_str(),"a");
      fprintf(pFile,"%s %d Poll end ",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str(),sec);
      fclose (pFile);
#endif
      // Signal new state if it what we expected or, hasn't become
      // what we expected after polls_to_stabilize polls. Unsolicited
      // changes will be signalled immediately unless they intervene
      // in a expected sequence where they will be delayed.
      if (retries_)
        {
          --retries_;
          if (state () == next_state_ || !retries_)
            {
              // the expected state has arrived or there are no more
              // retries
              force_signal = true;
            }
        }
      else if (state () != last_signalled_state_)
        {
          // here is the normal passive polling path where state has
          // changed asynchronously
          force_signal = true;
        }

      if (force_signal)
        {
          // reset everything, record and signal the current state
          retries_ = 0;
          next_state_ = state ();
          last_signalled_state_ = state ();
#if JTDX_DEBUG_TO_FILE
          pFile = fopen (debug_file_.c_str(),"a");
          fprintf(pFile,"%s signal\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str());
          fclose (pFile);
#endif
          update_complete (true);
        }
      else {
#if JTDX_DEBUG_TO_FILE
        pFile = fopen (debug_file_.c_str(),"a");
        fprintf(pFile,"%s no signal\n",QDateTime::currentDateTimeUtc().toString("hh:mm:ss.zzz").toStdString().c_str());
        fclose (pFile);
#endif
      }
    }
  catch (std::exception const& e)
    {
      message = e.what ();
    }
  catch (...)
    {
      message = tr ("Unexpected rig error");
    }
  if (!message.isEmpty ())
    {
      offline (message);
    }
}
