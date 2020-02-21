/*
 * maintains QSO Histories and autoselect
 * ES1JA last modified 07.02.2020
 */

#include "qsohistory.h"
void QsoHistory::init()
{
    _data.clear();
    _blackdata.clear();
    _working = true;
    _CQ.call = "";    
    _CQ.status = NONE;
    _CQ.srx_c = NONE;
    _CQ.srx_p = NONE;
    _CQ.stx_c = NONE;
    _CQ.stx_p = NONE;
    _CQ.count = 0;
    _CQ.b_time = 0;
    _CQ.time = 0;
    _CQ.rx = 0;
    _CQ.distance = 0;
}

QsoHistory::latlng QsoHistory::fromQth(QString const& qth) {
    latlng mylatlng;
    mylatlng.lat=0;
    mylatlng.lng=0;
    QString myqth;
    auto qthLen = qth.size();
    if (qthLen < 4)  myqth = qth.left(2).toUpper() + "55LL55LL";
    else if (qthLen < 6)  myqth = qth.left(4).toUpper() + "LL55LL";
    else if (qthLen < 8)  myqth = qth.left(6).toUpper() + "55LL";
    else if (qthLen < 10) myqth+= qth.left(8).toUpper() + "LL";
    else myqth = qth.toUpper();
    if (_gridRe.match(myqth).hasMatch()) {
      int l [10];
      for(int i=0; i < myqth.length(); i++) l[i] = myqth.at(i).toLatin1() - 65; 
      l[2] += 17; 
      l[3] += 17;
      l[6] += 17; 
      l[7] += 17;
      mylatlng.lng = (l[0]*rad_0 + l[2]*rad_2 + l[4]*rad_4 + l[6]*rad_6 + (l[8]+0.5)*rad_8 - M_PI);
      mylatlng.lat = (l[1]*rad_1 + l[3]*rad_3 + l[5]*rad_5 + l[7]*rad_7 + (l[9]+0.5)*rad_9 - M_PI_2);
    }    
    return mylatlng;
}

int QsoHistory::Distance(latlng latlng1,latlng latlng2) {
    if ((latlng1.lat == 0 && latlng1.lng  == 0) || (latlng2.lat == 0 && latlng2.lng  == 0)) return 0;
    else {
        double dlon = latlng2.lng - latlng1.lng;
        double dlat = latlng2.lat - latlng1.lat;
        double a = qSin(dlat / 2.0) * qSin(dlat / 2.0) + qCos(latlng1.lat) *
                  qCos(latlng2.lat) * qSin(dlon / 2.0) * qSin(dlon / 2.0);
        double c = 2.0 * qAtan2(qSqrt(a), qSqrt(1.0 - a));
        return 6378.137 * c;
    }
}


int QsoHistory::remove(QString const& callsign)
{
  int ret=0,ret2=0;
  if (_working) {
    ret=_data.remove(Radio::base_callsign (callsign));
    ret2=_blackdata.remove(Radio::base_callsign (callsign));
  }
  return ret;
}

int QsoHistory::blacklist(QString const& callsign)
{
  int ret=0;
  if (_working) {
    ret = _blackdata.value(Radio::base_callsign (callsign),ret);
    ret += 1;
    _blackdata.insert(Radio::base_callsign (callsign),ret);
  }
  return ret;
}

int QsoHistory::reset_count(QString const& callsign,Status status)
{
  int ret=0;
  if (_working) {
        QSO t;
        t = _data.value(Radio::base_callsign (callsign),t);
        if (t.call == callsign) {
            t.count = 0;
            if (status != NONE) t.status = status;
            _data.insert(Radio::base_callsign (callsign),t);
            ret = 1;
        }
  }
  return ret;
}

int QsoHistory::autoseq(QString &callsign, QString &grid, Status &status, QString &rep, int &rx, int &tx, unsigned &time, int &count, int &prio)
{
    if (_working)
    {
      int ret = 0;
      bool myas_active = as_active;
      if (callsign.length() > 2)
        {
          int hound = count;
          QSO t;
          t.status = NONE;
          t.rx = 0;
          t = _data.value(Radio::base_callsign (callsign),t);
          if (t.status > NONE || t.rx > 0) {
            status = t.status;
            prio = t.priority;
            if (hound == -1) {
              if(t.stx_c == SCALL && status == RREPORT) count = 1;
              else count = t.count + 1;
            } else if(((t.tyyp.size () == 2 && t.tyyp != mycontinent_ && t.tyyp != _CQ.call.left(2) && t.tyyp != myprefix_ && (t.tyyp != "DX" || t.continent == mycontinent_)) || 
                    (t.tyyp.size () == 1 && t.tyyp != _CQ.call.left(1) && t.tyyp != myprefix_)) && (!_strictdirCQ || (t.priority < 20 && t.status != RCQ))) {
              count = 1;    
            } else count = t.count;
            if (t.grid.length() >3) grid = t.grid;
            if (!t.s_rep.isEmpty ()) rep = t.s_rep;
            if (t.rx >0) rx = t.rx;
            if (t.tx >0) tx = t.tx;
            if (t.b_time > 0) time = t.b_time;
            if ((status >= S73 || status == SRR73) && tx == _CQ.tx) t.direction = 1;
            ret = t.direction;
            if (hound == -1 && status == RRR73) {
              t.status = FIN;
              _data.insert(Radio::base_callsign (callsign),t);
            }
          }
          myas_active = false;
//          as_active = false;
          return ret;
        }
      else
        { 
          algo=time;
          dist = 0;
          if (algo & 128) a_init=-1;
          else a_init=4;
          if (algo & 64) b_init=-1;
          else b_init=4;
          if (myas_active && _data.size() > 0) { //my CQ answers && _CQ.count > 0 
            QSO tt,t;
            int priority = a_init;
            rep = "-60";
            dist = 0;
            Rrep = "-60";
            bool mycall = false;
            foreach(QString key,_data.keys()) {
              ret=_blackdata.value(key,0);
              tt=_data[key];
              if (ret == 0 && tt.time == max_r_time && (tt.status == RCALL || tt.status == RREPORT || ((tt.status == RCQ || tt.status == RFIN) && !mycall && ((tt.priority > 16 && tt.priority < 20) || (tt.priority > 1 && tt.priority < 5)))) && !tt.continent.isEmpty()) {
                if (tt.priority > priority || 
                      (priority > a_init && (((tt.status == RCALL || tt.status == RREPORT) && !mycall) || (tt.priority == priority &&
                         ((!(algo & 32) && ((!(algo & 16) && !tt.s_rep.isEmpty () && tt.s_rep.toInt() > rep.toInt())
                                            || (algo & 16 && ((tt.status == RCALL && !tt.s_rep.isEmpty () && tt.s_rep.toInt() > rep.toInt() && Rrep == "-60")
                                                              ||(tt.status == RREPORT && !tt.s_rep.isEmpty () && tt.s_rep.toInt() > Rrep.toInt()))))) 
                          || (algo & 32 && tt.distance > dist)))))) {
                  if(_CQ.tyyp.isEmpty () || (_strictdirCQ && (tt.priority > 16 || (tt.priority > 1 && tt.priority < 5))) || _CQ.tyyp == tt.continent || _CQ.tyyp == tt.mpx || tt.call.startsWith(_CQ.tyyp) || (_CQ.tyyp == "DX" && tt.continent != mycontinent_)) {
                    t = tt;
                    if (tt.status == RCALL || tt.status == RREPORT) mycall = true;
                    priority = tt.priority;
                    prio = tt.priority;
                    status = tt.status;
                    callsign = tt.call;
                    count = tt.count;
                    dist = tt.distance;
//                    if (tt.grid.length() >3) grid = tt.grid;
                    grid = tt.grid;
                    if (!tt.s_rep.isEmpty ()) {
                      if (tt.status == RREPORT && (algo & 16)) Rrep = tt.s_rep;
                      rep = tt.s_rep;
                    }
                    if (tt.rx >0) rx = tt.rx;
                    if (tt.tx >0) tx = tt.tx;
                    if (tt.status == RREPORT && tt.direction == 1) tt.b_time = max_r_time;
                    time = tt.b_time;
                    myas_active = false;
                    as_active = false;
                  }
                }
              }
            }
            if (!myas_active) {
              return t.direction;    
            }
          }
          if (algo&1 && myas_active && _data.size() > 0){ // their CQ answers

            QSO tt,t;
            int priority = b_init;
            rep = "-60";
            dist = 0;
            foreach(QString key,_data.keys()) {
              ret=_blackdata.value(key,0);
              tt=_data[key];
              if (ret == 0 && tt.time == max_r_time && (tt.status == RCQ || (tt.status == RFIN && tt.priority > 0)) && !tt.continent.isEmpty()) {
                if (tt.priority > priority || 
                    (priority > b_init && tt.priority == priority && 
                    ((!(algo & 32) && !tt.s_rep.isEmpty () && tt.s_rep.toInt() > rep.toInt())
                    || (algo & 32 && tt.distance > dist)))) {
                  if((tt.tyyp.isEmpty () || tt.tyyp.size () > 2 || (_strictdirCQ && tt.priority > 19) || tt.tyyp == mycontinent_ || _CQ.call.startsWith(tt.tyyp) || tt.tyyp == myprefix_ || (tt.tyyp == "DX" && tt.continent != mycontinent_))
                  && (_CQ.tyyp.isEmpty () || _CQ.tyyp == tt.continent || (_strictdirCQ && (tt.priority > 16 || (tt.priority > 1 && tt.priority < 5))) || tt.call.startsWith(_CQ.tyyp) || (_CQ.tyyp == "DX" && tt.continent != mycontinent_))) {
                    t = tt;
                    priority = tt.priority;
                    prio = tt.priority;
                    status = tt.status;
                    callsign = tt.call;
                    count = tt.count;
                    dist = tt.distance;
//                    if (tt.grid.length() >3) grid = tt.grid;
                    grid = tt.grid;
                    if (!tt.s_rep.isEmpty ()) rep = tt.s_rep;
                    if (tt.rx >0) rx = tt.rx;
                    if (tt.tx >0) tx = tt.tx;
                    time = tt.b_time;
                  }
                }
              }
            }
            myas_active = false;
            as_active = false;
            return t.direction;    
            
          }
        }
    }
as_active = false;
return 0;
}

QsoHistory::Status QsoHistory::log_data(QString const& callsign, unsigned &time, QString &rrep, QString &srep)
{
    if (_working)
    {
        QSO t;
        t.call = callsign;
        t.status = NONE;
        t.b_time = 0;
        t.r_rep = "";
        t.s_rep ="";
        t = _data.value(Radio::base_callsign (callsign),t);
        time = t.b_time;
        rrep = t.r_rep;
        srep = t.s_rep;        
        return t.status;
    } else return NONE;
}

void QsoHistory::time(unsigned time)
{
    if (_working) {
        max_r_time = time;
    }
}

QsoHistory::Status QsoHistory::status(QString const& callsign, QString &grid)
{
    if (_working)
    {
        QSO t;
        t.call = callsign;
        t.status = NONE;
        t.grid = "";
        t = _data.value(Radio::base_callsign (callsign),t);
        grid = t.grid;
        return t.status;
    } else return NONE;
}

void QsoHistory::owndata(QString const& mycontinent, QString const& myprefix, QString const& mygrid, bool strictdirCQ)
{
    if (_working)
    {
        _strictdirCQ = !strictdirCQ;
        mycontinent_=mycontinent.trimmed();
        myprefix_ = myprefix;
        _mylatlng = fromQth(mygrid);
    }
}

void QsoHistory::rx(QString const& callsign,int freq)
{
    if (_working)
    {
        QSO t;
        t.call = callsign;
        t.status = NONE;
        t.rx = 0;
        t.time = 0;
        t = _data.value(Radio::base_callsign (callsign),t);
        if (t.time != max_r_time && t.rx != freq && freq > 0) {
            t.rx = freq;
            _data.insert(Radio::base_callsign (callsign),t);
        }
    }
}

void QsoHistory::message(QString const& callsign, Status status, int priority, QString const& param, QString const& tyyp, QString const& continent, QString const& mpx, unsigned time, QString const& rep, int freq)
{
    if (_working)
    {
      if (status == SCQ)
        {
          _CQ.call = callsign;
          _CQ.status = status;
          _CQ.stx_p = _CQ.stx_c;
          _CQ.stx_c = status;
          _CQ.grid = param;
          _CQ.tyyp = tyyp;
          _CQ.continent = mycontinent_;
          _CQ.mpx = myprefix_;
          _CQ.time = _CQ.b_time = time;
          _CQ.tx = freq;
          _CQ.priority = priority;
          _CQ.count += 1;
          _CQ.direction = 1;
        }
      else
        {
          Status old_status = NONE;
          QSO t;
          t.status = NONE;
          t.srx_c = NONE;
          t.srx_p = NONE;
          t.stx_c = NONE;
          t.stx_p = NONE;
          t.count = 0;
          t.priority = 0;
          t.tx = 0;
          t.rx = 0;
          t.b_time = 0;
          t.call = callsign;
          t.direction = 0;
          t.time = 0;
          t.tyyp = "";
          t.continent = "";
          t.mpx = "";
          t.grid = "";
          t.r_rep = "";
          t.s_rep = "";
          t.distance = 0;
          t = _data.value(Radio::base_callsign (callsign),t);
          if (time >= t.time || time == 0 || status >= t.status || status == RREPORT) {
            if (status > NONE) {
              t.time = time;
              if (status == SCALL || status == SREPORT || status == SRR || status == SRR73 || status == S73) {
                  if (t.status == NONE && (status == SCALL || (status == SREPORT && tyyp == "S"))) {
                    t.status = RCQ;
                    t.srx_c = RCQ;
                  }
                  if (status == t.stx_c) t.count += 1;
                  else {
                    printf ("reset count %d to 1 %d,%d,%d\n",t.count,t.stx_c,status,t.direction); 
                    t.count = 1;
                  }
                  _CQ.count = 0;
              } else if (status > RCQ && status != t.srx_c) {
                printf ("reset count %d to 0 %d,%d,%d\n",t.count,t.srx_c,status,t.direction); 
                t.count = 0;
              }
            }
            if (t.call.length() < callsign.length() && callsign.contains(t.call))
              {
                t.call=callsign;
              }
            switch (status)
            {
              case NONE:
                {
                  if (!param.isEmpty()) {
                    t.grid = param;
                    t.distance=Distance(_mylatlng,fromQth(param));
                  }
                  break;
                }
              case RFIN:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
  //                if (t.status <= SREPORT)
                  if (t.status <= SCQ || t.status == SCALL || 
                      (t.status == RCALL && t.time != time) ||
                      (t.status > SCQ && t.status < SRR73 && t.time - t.b_time > 300)) // an attempt to support CQ and any other message reception from MSHV multislot operation mode
                    {
                      old_status = t.status;
                      t.status = status;
                      t.priority = priority;
                      if (t.continent == "") t.tyyp = tyyp;
                      if (t.continent == "") t.continent = continent.trimmed();
                      if (t.mpx == "") t.mpx = mpx;
                      t.b_time = time;
                      t.s_rep = rep;
                      t.rx = freq;
                      t.direction = 0;
                    }  
  //                max_r_time = time;
                  as_active = true;
                  break;
                }
              case RCQ:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
  //                if (t.status <= SREPORT)
                  if (t.status <= SCQ || t.status == SCALL || 
                      (t.status == RCALL && t.time != time) ||
                      (t.status > SCQ && t.status < SRR73 && t.time - t.b_time > 300)) // an attempt to support CQ and any other message reception from MSHV multislot operation mode
                    {
                      old_status = t.status;
                      t.status = status;
                      t.priority = priority;
                      if (!param.isEmpty()) {
                        t.grid = param;
                        t.distance=Distance(_mylatlng,fromQth(param));
                      }
                      if (tyyp == "CQ" || tyyp == "LP")
                        t.tyyp = "";
                      else
                        t.tyyp = tyyp;
                      t.continent = continent.trimmed();
                      t.mpx = mpx;
                      t.b_time = time;
                      t.s_rep = rep;
                      t.rx = freq;
                      t.direction = 0;
                    }  
  //                max_r_time = time;
                  as_active = true;
                  break;
                }
              case RCALL:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
                  t.direction = 1;
                  old_status = t.status;
                  t.status = status;
                  t.priority = priority;
                  if (!param.isEmpty()) {
                    t.grid = param;
                    t.distance=Distance(_mylatlng,fromQth(param));
                  }
                  t.continent = continent.trimmed();
                  t.mpx = mpx; 
                  t.b_time = time;
                  t.s_rep = rep;
                  t.rx = freq;
  //                max_r_time = time;
                  as_active = true;
                  break;
                }
              case SCALL:
                {
                  t.stx_p = t.stx_c;
                  t.stx_c = status;
  //                if (t.status <= SCALL ) {
                    old_status = t.status;
                    t.status = status;
                    t.b_time = time;
                    t.tx = freq;
                    t.direction = 0;
  //                }
                  break;
                            }
              case RREPORT:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
                  if (t.status < FIN) {
                      if (t.status == RCALL || (t.status == SREPORT && t.r_rep == param)) t.direction = 0;
                      if  (t.status <= status || t.direction == 0) t.s_rep = rep;
                      old_status = t.status;
                      t.status = status;
                      if (priority > t.priority) t.priority = priority;
                      if (t.continent.isEmpty ()) t.continent = continent.trimmed();
                      if (t.mpx.isEmpty ()) t.mpx = mpx;
                      t.r_rep = param;
                      if (t.b_time == 0 || old_status < SCQ) t.b_time = time;
                      t.rx = freq;
      //                max_r_time = time;
                      as_active = true;
                  }
                  break;
                }
              case SREPORT:
                {
                  t.stx_p = t.stx_c;
                  t.stx_c = status;
                  if (t.status <= SREPORT ) {
                    if (t.status <= RCQ || t.r_rep.isEmpty () || (t.status == RREPORT && tyyp == "S")) {
                      t.direction = 1;
                    } 
                    t.s_rep = param;
                    old_status = t.status;
                    t.status = status;
                    t.tx = freq;
                    if (t.b_time == 0 || old_status < SCQ) t.b_time = time;
                  }
                  break;
                }
              case RRR:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
                  if (!t.r_rep.isEmpty () && !t.s_rep.isEmpty () && t.status < FIN)
                    {
                      old_status = t.status;
                      t.status = status;
                      t.rx = freq;
                    }
  //                max_r_time = time;
                  break;
                }
              case SRR:
                {
                  t.stx_p = t.stx_c;
                  t.stx_c = status;
                  if (t.status <= SRR ) {
                    old_status = t.status;
                    t.status = status;
                    t.tx = freq;
                  }
                  break;
                }
              case RRR73:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
                  if(!t.r_rep.isEmpty () && !t.s_rep.isEmpty () && t.status >= SREPORT)
                    {
                      if (t.status == SRR73 || (t.srx_p != RRR73 && t.status == FIN))
                        {
                          old_status = t.status;
                          t.status = FIN;
                          t.priority = 0;
                        }
                      else 
                        {
                          old_status = t.status;
                          t.status = status;
                        }
                      t.rx = freq;
                    }
  //                max_r_time = time;
                  break;
                }
              case SRR73:
                {
                  t.stx_p = t.stx_c;
                  t.stx_c = status;
                  if (t.status == RRR73 || t.status == R73 || t.status == FIN)
                    {
                      old_status = t.status;
                      t.status = FIN;
                      t.priority = 0;
                    }
                  else
                    {
                      old_status = t.status;
                      t.status = status;
                    }
                  t.tx = freq;
                  break;
                }
              case R73:
                {
                  t.srx_p = t.srx_c;
                  t.srx_c = status;
                  if(!t.r_rep.isEmpty () && !t.s_rep.isEmpty () && t.status >= SREPORT)
                    {
                      if (t.status == SRR73 || t.status == S73 || t.status == FIN)
                        {
                          old_status = t.status;
                          t.status = FIN;
                          t.priority = 0;
                        }
                      else
                        {
                          old_status = t.status;
                          t.status = status;
                        }
                      t.rx = freq;
                    }
  //                max_r_time = time;
                  break;
                }
              case S73:
                {
                  t.stx_p = t.stx_c;
                  t.stx_c = status;
                  if (t.status == RRR73 || t.status == R73 || t.status == FIN)
                    {
                      old_status = t.status;
                      t.status = FIN;
                      t.priority = 0;
                    }
                  else
                    {
                      old_status = t.status;
                      t.status = status;
                    }
                  t.tx = freq;
                  break;
                }
              default:
                {
                  old_status = t.status;
                  t.status = status;
                  break;
                }
            }
            _data.insert(Radio::base_callsign (callsign),t);
          }
          
        }
    }
      
}     
      
