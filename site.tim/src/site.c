/* site.c
   ====== 
   Author R.J.Barnes
*/
/*
 $License$
*/


#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>
#include <libconfig.h>
#include "rtypes.h"
#include "limit.h"
#include "tsg.h"
#include "maketsg.h"
#include "acf.h"
#include "acfex.h"
#include "tcpipmsg.h"
#include "rosmsg.h"
#include "shmem.h"
#include "global.h"
#include "site.h"
#include "siteglobal.h"

#define REAL_BUF_OFFSET 0
#define IMAG_BUF_OFFSET 1
#define USEC 1000000.0

/* TODO: PUT STATIC OFFSET INTO DMAP TO ACCOUNT FOR OFFSET BETWEEN DDS AND RX */ 

config_t cfg;
char *config_dir=NULL;
char config_filepath[256]="/tmp/tst.cfg";
char channame[5]="\0";

FILE *seqlog=NULL;
char seqlog_name[256];
char *seqlog_dir=NULL;

FILE *msglog=NULL;
char msglog_name[256];
char *msglog_dir=NULL;

FILE *f_diagnostic_ascii=NULL,*fp=NULL;

int yday=-1;
int iqbufsize=0;

void SiteTimExit(int signum) {

  struct ROSMsg msg;
  switch(signum) {
    case 2:
      if (debug) printf("SiteTimExit: Sig %d: %d\n",signum,exit_flag); 
      cancel_count++;
      exit_flag=signum;
      if (cancel_count < 3 )
        break;
    case 0:
      if (debug) printf("SiteTimExit: Sig %d: %d\n",signum,exit_flag); 
      if(exit_flag!=0) {
        msg.type=QUIT;
        TCPIPMsgSend(sock, &msg, sizeof(struct ROSMsg));
        TCPIPMsgRecv(sock, &msg, sizeof(struct ROSMsg));
        if (debug) {
          fprintf(stderr,"QUIT:type=%c\n",msg.type);
          fprintf(stderr,"QUIT:status=%d\n",msg.status);
        }
        close(sock);
        if(seqlog!=NULL) {
          fflush(seqlog);
          fclose(seqlog);
          seqlog=NULL;
        } 
        if(msglog!=NULL) {
          fclose(msglog);
          msglog=NULL;
        } 
        if(f_diagnostic_ascii!=NULL) {
          fclose(f_diagnostic_ascii);
          f_diagnostic_ascii=NULL;
        }
        if (samples !=NULL)
          ShMemFree((unsigned char *) samples,sharedmemory,iqbufsize,1,shmemfd);
        exit(errno);
      } 
      break;
    default:
      if (debug) printf("SiteTimExit: Sig %d: %d\n",signum,exit_flag); 
      if(exit_flag==0) {
        exit_flag=signum;
      }
      if(exit_flag!=0) {
        msg.type=QUIT;
        TCPIPMsgSend(sock, &msg, sizeof(struct ROSMsg));
        TCPIPMsgRecv(sock, &msg, sizeof(struct ROSMsg));
        if (debug) {
          fprintf(stderr,"QUIT:type=%c\n",msg.type);
          fprintf(stderr,"QUIT:status=%d\n",msg.status);
        }
        close(sock);
        if(seqlog!=NULL) {
          fflush(seqlog);
          fclose(seqlog);
          seqlog=NULL;
        } 
        if(msglog!=NULL) {
          fclose(msglog);
          msglog=NULL;
        } 
        if(f_diagnostic_ascii!=NULL) {
          fclose(f_diagnostic_ascii);
          f_diagnostic_ascii=NULL;
        }
        config_destroy (&cfg );
        if (samples !=NULL)
          ShMemFree((unsigned char *) samples,sharedmemory,iqbufsize,1,shmemfd);
        exit(errno);
      }

      break;
  }
}



int SiteTimStart(char *host,char *ststr) {
  int retval;
  long ltemp;
  const char *str;
  char *dfststr="tst";
  char *chanstr=NULL;
  signal(SIGPIPE,SiteTimExit);
  signal(SIGINT,SiteTimExit);
  signal(SIGUSR1,SiteTimExit);

  for(nave=0;nave<MAXNAVE;nave++) {
    seqbadtr[nave].num=0;
    seqbadtr[nave].start=NULL;
    seqbadtr[nave].length=NULL;
  }
  nave=0;
  rdata.main=NULL;
  rdata.back=NULL;
  badtrdat.start_usec=NULL;
  badtrdat.duration_usec=NULL;
  tsgbuf=NULL;
  tsgprm.pat=NULL;
  samples=NULL;
  exit_flag=0;
  cancel_count=0;
  sock=0;

  /* use libconfig and read in configuration file to set global rst variables to site specific values
  *    which are appropriate for the site this library is being called to manage.
  *    Using just this library function and configuration file, it should be possible to run the
  *    standard ros provided normalscan controlprogram with no additional arguments, relying entirely on
  *    system environment settings and the site configuration file.
  *
  *    The intent here is that controlprograms will then optionally override some or all of these values via
  *    commandline arguments, env variables, or other configuration means.
  *  
  */ 
  config_dir=getenv("SITE_CFG");

  printf("Config_Dir: %s\n",config_dir);
  if (config_dir==NULL) {
    fprintf(stderr,"SITE_CFG environment variable is unset\nSiteTimStart aborting, controlprogram should end now\n");
    return -1;
  }
/* TODO: implement chanstr handling as an option to allow for chan specific defaults */
  if(chanstr==NULL) chanstr=getenv("CHANSTR");
  if(ststr==NULL) ststr=getenv("STSTR");
  if(ststr==NULL) ststr=dfststr;
  printf("StStr: %s ChanStr %s\n",ststr,chanstr);

  if(chanstr==NULL) {
    sprintf(config_filepath,"%s/site.%s/%s.cfg",config_dir,ststr,ststr);
  } else {
    sprintf(config_filepath,"%s/site.%s/%s.%s.cfg",config_dir,ststr,ststr,chanstr);
  }
  fprintf(stdout,"Opening config file: %s\n",config_filepath);
  config_init (&cfg );
  retval=config_read_file(&cfg,config_filepath);
  if (retval==CONFIG_FALSE) {
    fprintf(stderr, "configfile read error:%d - %s\n", 
            config_error_line(&cfg), config_error_text(&cfg));
    /* A config error read has occured */
  }
  /* Get the station name. */
  if(config_lookup_string(&cfg, "station", &str)){
    fprintf(stdout,"Station name: %s\n\n", str);
    strcpy(station,str);
  } else {
    fprintf(stderr, "Site Cfg Error:: No 'station' setting in configuration file.\nSiteTimStart aborting, controlprogram should end now\n");
    return -1;
  }
  if(! config_lookup_int(&cfg, "match_filter", &ltemp)) {
    dmatch=0;
    fprintf(stderr,"Site Cfg Warning:: \'match_filter\' setting undefined in site cfg file using default value: %d\n",dmatch); 
  } else {
    dmatch=ltemp;
  }
  if(! config_lookup_int(&cfg, "backward", &ltemp)) {
    backward=0;
    fprintf(stderr,"Site Cfg Warning:: \'backward\' setting undefined in site cfg file using default value: %d\n",backward); 
  } else {
    backward=ltemp;
  }
  if(! config_lookup_int(&cfg, "xcf", &ltemp)) {
    /* xcf count value: 1 means every integration period. 2 means every other. N means every Nth.*/
    xcnt=0;
    fprintf(stderr,"Site Cfg Warning:: \'xcf\' setting undefined in site cfg file using default value: %d\n",xcnt); 
  } else {
    xcnt=ltemp;
    fprintf(stderr,"Site Cfg:: \'xcf\' setting in site cfg file using value: %d\n",xcnt); 
  }
  if(! config_lookup_int(&cfg, "sbm", &ltemp)) {
    sbm=0;
    fprintf(stderr,"Site Cfg Warning:: \'sbm\' setting undefined in site cfg file using default value: %d\n",sbm); 
  } else {
    sbm=ltemp;
    fprintf(stderr,"Site Cfg:: \'sbm\' setting in site cfg file using value: %d\n",sbm); 
  }
  if(! config_lookup_int(&cfg, "ebm", &ltemp)) {
    ebm=16;
    fprintf(stderr,"Site Cfg Warning:: \'ebm\' setting undefined in site cfg file using default value: %d\n",ebm); 
  } else {
    ebm=ltemp;
    fprintf(stderr,"Site Cfg:: \'ebm\' setting in site cfg file using value: %d\n",ebm); 
  }
  if(! config_lookup_int(&cfg, "rnum", &ltemp)) {
    /* Radar number to register  with ROS server*/
    rnum=1;
    fprintf(stderr,"Site Cfg Warning:: \'rnum\' setting undefined in site cfg file using default value: %d\n",rnum); 
  } else {
    rnum=ltemp;
  }
  if(! config_lookup_int(&cfg, "cnum", &ltemp)) {
    /* Channum to register  with ROS server*/
    cnum=1;
    fprintf(stderr,"Site Cfg Warning:: \'cnum\' setting undefined in site cfg file using default value: %d\n",cnum); 
  } else {
    cnum=ltemp;
  }
  if(! config_lookup_string(&cfg, "ros.host", &str)) {
    /* ROS server tcp port*/
    if(host!=NULL) {
      strcpy(server,host);
    } else {
      strcpy(server,"127.0.0.1");
    }
    fprintf(stderr,"Site Cfg Warning:: \'ros.host\' setting undefined in site cfg file using default value: \'%s\'\n",server); 
  } else{
    strcpy(server,str);
  }
  if(! config_lookup_int(&cfg, "ros.port", &ltemp)) {
    /* ROS server tcp port*/
    port=45000;
    fprintf(stderr,"Site Cfg Warning:: \'ros.port\' setting undefined in site cfg file using default value: %d\n",port); 
  } else {
    port=ltemp;
  }
  if(! config_lookup_int(&cfg, "errlog.port", &ltemp)) {
    /* ROS server tcp port*/
    errlog.port=45000;
    fprintf(stderr,"Site Cfg Warning:: \'errlog.port\' setting undefined in site cfg file using default value: %d\n",errlog.port); 
  } else {
    errlog.port=ltemp;
  }
  if(! config_lookup_string(&cfg, "errlog.host", &str)) {
    /* ROS server tcp port*/
    strcpy(errlog.host,"127.0.0.1");
    fprintf(stderr,"Site Cfg Warning:: \'errlog.host\' setting undefined in site cfg file using default value: \'%s\'\n",errlog.host); 
  } else{
    strcpy(errlog.host,str);
  }
  if(! config_lookup_int(&cfg, "shellserver.port", &ltemp)) {
    /* ROS server tcp port*/
    shell.port=45001;
    fprintf(stderr,"Site Cfg Warning:: \'shellserver.port\' setting undefined in site cfg file using default value: %d\n",shell.port); 
  } else {
    shell.port=ltemp;
  }
  if(! config_lookup_string(&cfg, "shellserver.host", &str)) {
    /* ROS server tcp port*/
    strcpy(shell.host,"127.0.0.1");
    fprintf(stderr,"Site Cfg Warning:: \'shellserver.host\' setting undefined in site cfg file using default value: \'%s\'\n",shell.host); 
  } else{
    strcpy(shell.host,str);
  }
  if(! config_lookup_int(&cfg, "tasks.baseport", &ltemp)) {
    /* ROS server tcp port*/
    baseport=45001;
    fprintf(stderr,"Site Cfg Warning:: \'tasks.baseport\' setting undefined in site cfg file using default value: %d\n",baseport); 
  } else {
    baseport=ltemp;
  }
  if(! config_lookup_int(&cfg, "invert", &ltemp)) {
/* 
 *  If you need to correct for inverted phase between main and inter rf signal 
 *  invert=0  No inversion necessary 
 *   invert=non-zero  Inversion necassary 
*/
    invert=1;
    fprintf(stderr,"Site Cfg Warning:: \'invert\' setting undefined in site cfg file using default value: %d\n",invert); 
  } else {
    invert=ltemp;
  }
  if(! config_lookup_int(&cfg, "rxchn", &ltemp)) {
/* rxchn number of channels typically 1*/
/* rngoff argument in ACFCalculate.. is 2*rxchn and is normally set to 2 */
    rxchn=1;
    fprintf(stderr,"Site Cfg Warning:: \'rxchn\' setting undefined in site cfg file using default value: %d\n",rxchn); 
  } else {
    rxchn=ltemp;
  }
  if(! config_lookup_int(&cfg, "day", &ltemp)) {
    day=18;
    fprintf(stderr,"Site Cfg Warning:: \'day\' setting undefined in site cfg file using default value: %d\n",day); 
  } else {
    day=ltemp;
  }
  if(! config_lookup_int(&cfg, "night", &ltemp)) {
    night=10;
    fprintf(stderr,"Site Cfg Warning:: \'night\' setting undefined in site cfg file using default value: %d\n",night); 
  } else {
    night=ltemp;
  }

  if(! config_lookup_int(&cfg, "dfrq", &ltemp)) {
    dfrq=10400;
    fprintf(stderr,"Site Cfg Warning:: \'dfrq\' setting undefined in site cfg file using default value: %d\n",dfrq); 
  } else {
    dfrq=ltemp;
    fprintf(stderr,"Site Cfg:: \'dfrq\' setting in site cfg file using value: %d\n",dfrq); 
  }
  if(! config_lookup_int(&cfg, "nfrq", &ltemp)) {
    nfrq=10400;
    fprintf(stderr,"Site Cfg Warning:: \'nfrq\' setting undefined in site cfg file using default value: %d\n",nfrq); 
  } else {
    nfrq=ltemp;
    fprintf(stderr,"Site Cfg:: \'nfrq\' setting in site cfg file using value: %d\n",nfrq); 
  }
  return 0;
}


int SiteTimSetupRadar() {

  int32 temp32,data_length;;
  char ini_entry_name[80];
  char requested_entry_type,returned_entry_type;
  struct ROSMsg smsg,rmsg;
  struct timeval currtime;
  time_t ttime;
  struct tm tstruct;

  if ((sock=TCPIPMsgOpen(server,port)) == -1) {
    return -1;
  }
  fprintf(stderr,"Rnum: %d Cnum: %d\n",rnum,cnum);
  smsg.type=SET_RADAR_CHAN;
  TCPIPMsgSend(sock, &smsg,sizeof(struct ROSMsg)); 
  temp32=rnum;
  TCPIPMsgSend(sock, &temp32, sizeof(int32)); 
  temp32=cnum;
  TCPIPMsgSend(sock, &temp32, sizeof(int32));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg)); 
  if (rmsg.status < 0) {
    fprintf(stderr,"Requested radar channel unavailable\nSleeping 1 second and exiting\n");
    sleep(1);
    SiteTimExit(-1);
  } 
  if (debug) {
    fprintf(stderr,"SET_RADAR_CHAN:type=%c\n",rmsg.type);
    fprintf(stderr,"SET_RADAR_CHAN:status=%d\n",rmsg.status);
  }
  smsg.type=QUERY_INI_SETTINGS;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  sprintf(ini_entry_name,"site_settings:ifmode");  
  requested_entry_type='b';  
  returned_entry_type=' ';  
  temp32=-1;
  ifmode=-1;
  data_length=strlen(ini_entry_name)+1;
  TCPIPMsgSend(sock, &data_length, sizeof(int32));
  TCPIPMsgSend(sock, &ini_entry_name, data_length*sizeof(char));
  TCPIPMsgSend(sock, &requested_entry_type, sizeof(char));
  TCPIPMsgRecv(sock, &returned_entry_type, sizeof(char));
  TCPIPMsgRecv(sock, &data_length, sizeof(int32));
  if((returned_entry_type==requested_entry_type)  ) {
    TCPIPMsgRecv(sock, &temp32, sizeof(int32));
  } 
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"QUERY_INI_SETTINGS:type=%c\n",rmsg.type);
    fprintf(stderr,"QUERY_INI_SETTINGS:status=%d\n",rmsg.status);
    fprintf(stderr,"QUERY_INI_SETTINGS:entry_name=%s\n",ini_entry_name);
    fprintf(stderr,"QUERY_INI_SETTINGS:entry_type=%c\n",returned_entry_type);
    fprintf(stderr,"QUERY_INI_SETTINGS:entry_value=%d\n",temp32);
  }
  if((rmsg.status) && (temp32>=0) ) ifmode=temp32;
  if((ifmode!=0) && (ifmode!=1)) {
    fprintf(stderr,"QUERY_INI_SETTINGS: Bad IFMODE)\n");
    exit(0); 
  }
  smsg.type=GET_PARAMETERS;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock, &rprm, sizeof(struct ControlPRM));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"GET_PARAMETERS:type=%c\n",rmsg.type);
    fprintf(stderr,"GET_PARAMETERS:status=%d\n",rmsg.status);
  }

  sprintf(sharedmemory,"IQBuff_ROS_%d_%d",rnum,cnum);

  iqbufsize = 2 * (mppul) * sizeof(int32) * 1e6 * (intsc+1) * nbaud / mpinc; /* calculate size of IQ buffer (JTK) */

  fprintf(stderr,"intc: %d, nbaud %d, mpinc %d, iq buffer size is %d\n",intsc, nbaud, mpinc, iqbufsize);
  samples = (int16 *)ShMemAlloc(sharedmemory,iqbufsize,O_RDWR | O_CREAT,1,&shmemfd);
  
  if(samples==NULL) { 
    fprintf(stderr,"IQBuffer %s is Null\n",sharedmemory);
    SiteTimExit(-1);
  }
/* Setup the seqlog file here*/
  gettimeofday(&currtime,NULL);
  ttime=currtime.tv_sec;
  gmtime_r(&ttime,&tstruct);
  switch(cnum) {
	case 1:
          strcpy(channame,".a");
          break;
	case 2:
          strcpy(channame,".b");
          break;
	case 3:
          strcpy(channame,".c");
          break;
	case 4:
          strcpy(channame,".d");
          break;
	default:
          strcpy(channame,"");
          break;
  }
  if (msglog!=NULL) {
      fclose(msglog);
      msglog=NULL;
  }
  msglog_dir = getenv("MSGLOG_DIR");
  if(msglog_dir!=NULL) { 
    fprintf(stdout,"msglog dir: %s\n",msglog_dir);
    sprintf(msglog_name,"%s/msglog.%s%s.%04d%02d%02d",msglog_dir,station,channame,tstruct.tm_year+1900,tstruct.tm_mon+1,tstruct.tm_mday);
    fprintf(stdout,"msglog filename: %s\n",msglog_name);
    msglog=fopen(msglog_name,"a+");
  } else {
    fprintf(stdout,"No msglog directory defined\n");
  }

  if (seqlog!=NULL) {
      fflush(seqlog);
      fclose(seqlog);
      seqlog=NULL;
  }
  seqlog_dir = getenv("SEQLOG_DIR");
  if(seqlog_dir!=NULL) { 
    fprintf(stdout,"seqlog dir: %s\n",seqlog_dir);
    sprintf(seqlog_name,"%s/seqlog.%s%s.%04d%02d%02d",seqlog_dir,station,channame,tstruct.tm_year+1900,tstruct.tm_mon+1,tstruct.tm_mday);
    fprintf(stdout,"seqlog filename: %s\n",seqlog_name);
    seqlog=fopen(seqlog_name,"a+");
  } else {
    fprintf(stdout,"No seqlog directory defined\n");
  }
  fflush(stdout);
  return 0;
}

 
int SiteTimStartScan(int32_t wait_seconds) {
  struct ROSMsg smsg,rmsg;
  struct timeval tick;
  int32_t count=0;
  double  minute_seconds;
  if (debug) {
    fprintf(stderr,"SiteTimStartScan: start\n");
  }
  gettimeofday(&tick,NULL);
  while(count < wait_seconds) {
    SiteTimExit(0);
    gettimeofday(&tick,NULL);
    minute_seconds=tick.tv_sec % 60;
    if (minute_seconds >= wait_seconds) {
      break;
    }
    if (debug) {
      fprintf(stderr,"SiteTimStartScan: Ping %d\n",count);
    }
    smsg.type=PING;
    TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
    TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
    count++; 
    sleep(1);
    gettimeofday(&tick,NULL);
    minute_seconds=tick.tv_sec % 60;
    if (minute_seconds >= wait_seconds) {
      break;
    }
  } 
  smsg.type=SET_ACTIVE;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"SiteTimStartScan: end\n");
  }

  return 0;
}



int SiteTimStartIntt(int sec,int usec) {

  struct ROSMsg smsg,rmsg;
  int total_samples=0;
  double secs;
  SiteTimExit(0);
  if (debug) {
    fprintf(stderr,"SiteTimStartInt: start\n");
  }
  total_samples=tsgprm.samples+tsgprm.smdelay;
  smsg.type=PING; 
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"PING:type=%c\n",rmsg.type);
    fprintf(stderr,"PING:status=%d\n",rmsg.status);
  }

  smsg.type=GET_PARAMETERS;  
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock, &rprm, sizeof(struct ControlPRM));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"GET_PARAMETERS:type=%c\n",rmsg.type);
    fprintf(stderr,"GET_PARAMETERS:status=%d\n",rmsg.status);
  }

  rprm.tbeam=bmnum;   
  rprm.tfreq=12000;   
  rprm.trise=5000;   
  rprm.baseband_samplerate=((double)nbaud/(double)txpl)*1E6; 
  rprm.filter_bandwidth=rprm.baseband_samplerate; 
  rprm.match_filter=dmatch;
  rprm.number_of_samples=total_samples+nbaud+10; 
  rprm.priority=cnum;
  rprm.buffer_index=0;

  smsg.type=SET_PARAMETERS;
  TCPIPMsgSend(sock,&smsg,sizeof(struct ROSMsg));
  TCPIPMsgSend(sock,&rprm,sizeof(struct ControlPRM));
  TCPIPMsgRecv(sock,&rmsg,sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"SET_PARAMETERS:type=%c\n",rmsg.type);
    fprintf(stderr,"SET_PARAMETERS:status=%d\n",rmsg.status);
  }

  secs=sec+(double)usec/1E6;
  if(secs > 0 ) {
    /* set tock to expected end of integration */
    if (gettimeofday(&tock,NULL)==-1) return -1;
    tock.tv_sec+=floor(secs);
    tock.tv_usec+=(secs-floor(secs))*1E6;
  } else {
    /* set tock to zero, indicating no integration requested */
    tock.tv_sec=0;
    tock.tv_usec=0;
  }
  if (debug) {
    fprintf(stderr,"SiteTimStartInt: end\n");
  }
  return 0;


}


int SiteTimFCLR(int stfreq,int edfreq) {
  int32 tfreq;
  struct ROSMsg smsg,rmsg;
  struct CLRFreqPRM fprm;
  int total_samples=0;

  SiteTimExit(0);

  total_samples=tsgprm.samples+tsgprm.smdelay;
  rprm.tbeam=bmnum;   
  rprm.tfreq=tfreq;   
  rprm.rfreq=tfreq;   
  rprm.trise=5000;   
  rprm.baseband_samplerate=((double)nbaud/(double)txpl)*1E6; 
  rprm.filter_bandwidth=rprm.baseband_samplerate; 
  rprm.match_filter=dmatch;
  rprm.number_of_samples=total_samples+nbaud+10; 
  rprm.priority=cnum;
  rprm.buffer_index=0;

  smsg.type=SET_PARAMETERS;
  TCPIPMsgSend(sock,&smsg,sizeof(struct ROSMsg));
  TCPIPMsgSend(sock,&rprm,sizeof(struct ControlPRM));
  TCPIPMsgRecv(sock,&rmsg,sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"SET_PARAMETERS:type=%c\n",rmsg.type);
    fprintf(stderr,"SET_PARAMETERS:status=%d\n",rmsg.status);
  }

  fprm.start=stfreq; 
  fprm.end=edfreq;  
  fprm.nave=20;  
  fprm.filter_bandwidth=250;  

  smsg.type=REQUEST_CLEAR_FREQ_SEARCH;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgSend(sock, &fprm, sizeof(struct CLRFreqPRM));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"REQUEST_CLEAR_FREQ_SEARCH:type=%c\n",rmsg.type);
    fprintf(stderr,"REQUEST_CLEAR_FREQ_SEARCH:status=%d\n",rmsg.status);
  }

  smsg.type=REQUEST_ASSIGNED_FREQ;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock,&tfreq, sizeof(int32)); 
  TCPIPMsgRecv(sock,&noise, sizeof(float));  
  TCPIPMsgRecv(sock,&rmsg, sizeof(struct ROSMsg)); 
  if (debug) {
    fprintf(stderr,"REQUEST_ASSIGNED_FREQ:type=%c\n",rmsg.status);
    fprintf(stderr,"REQUEST_ASSIGNED_FREQ:status=%d\n",rmsg.status);
  }

  return tfreq;
}



int SiteTimTimeSeq(int *ptab) {

  int i;
  int flag,index=0;
  struct ROSMsg smsg,rmsg;
  int32_t parr[1]={1};
  struct SeqPRM tprm;
  SiteTimExit(0);
  if (tsgbuf !=NULL) TSGFree(tsgbuf);
  if (tsgprm.pat !=NULL) free(tsgprm.pat);
  memset(&tsgprm,0,sizeof(struct TSGprm));

  tsgprm.nrang=nrang;         
  tsgprm.frang=frang;
  tsgprm.rtoxmin=0;      
  tsgprm.stdelay=18+2;
  tsgprm.gort=1;
  tsgprm.rsep=rsep;          
  tsgprm.smsep=smsep;
  tsgprm.txpl=txpl; 
  tsgprm.mpinc=mpinc;
  tsgprm.mppul=mppul; 
  tsgprm.mlag=0;
  tsgprm.nbaud=nbaud;
  tsgprm.code=pcode;
  tsgprm.pat=malloc(sizeof(int32_t)*tsgprm.mppul);
  for (i=0;i<tsgprm.mppul;i++) tsgprm.pat[i]=ptab[i];

  tsgbuf=TSGMake(&tsgprm,&flag);

  if (tsgbuf==NULL) return -1;
  tprm.index=index;
  tprm.len=tsgbuf->len;
  tprm.step=CLOCK_PERIOD;
  tprm.samples=tsgprm.samples;
  tprm.smdelay=tsgprm.smdelay;
  tprm.nrang=tsgprm.nrang;
  tprm.frang=tsgprm.frang;
  tprm.rsep=tsgprm.rsep;
  tprm.smsep=tsgprm.smsep;
  tprm.lagfr=tsgprm.lagfr;
  tprm.txpl=tsgprm.txpl;
  tprm.mppul=tsgprm.mppul;
  tprm.mpinc=tsgprm.mpinc;
  tprm.mlag=tsgprm.mlag;
  tprm.nbaud=tsgprm.nbaud;
  tprm.stdelay=tsgprm.stdelay;
  tprm.gort=tsgprm.gort;
  tprm.rtoxmin=tsgprm.rtoxmin;
  
  smsg.type=REGISTER_SEQ;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgSend(sock, &tprm, sizeof(struct SeqPRM));
  TCPIPMsgSend(sock, tsgbuf->rep, sizeof(unsigned char)*tprm.len);
  TCPIPMsgSend(sock, tsgbuf->code, sizeof(unsigned char)*tprm.len);
  TCPIPMsgSend(sock, tsgprm.pat, sizeof(int32_t)*tprm.mppul);
  if (nbaud > 1) {
    TCPIPMsgSend(sock, tsgprm.code, sizeof(int32_t)*tprm.nbaud);
  } else {
    TCPIPMsgSend(sock, parr, sizeof(int32_t)*tprm.nbaud);
  }
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
  if (debug) {
    fprintf(stderr,"REGISTER_SEQ:type=%c\n",rmsg.type);
    fprintf(stderr,"REGISTER_SEQ:status=%d\n",rmsg.status);
  }

  if (rmsg.status !=1) return -1;

  lagfr=tsgprm.lagfr;
  smsep=tsgprm.smsep;
  txpl=tsgprm.txpl;

  return index;
}

int SiteTimIntegrate(int (*lags)[2], int32_t rfreq) {

  int *lagtable[2]={NULL,NULL};
  int lagsum[LAG_SIZE];

  int badrng=0;
  int i,j;
  int roff=REAL_BUF_OFFSET;
  int ioff=IMAG_BUF_OFFSET;
  int rngoff=2;

  struct timeval tick;
  struct timeval tack;
  double tval=0,tavg=0;
  double time_diff=0;
  struct tm tstruct;
  time_t ttime;
  char filename[256];
  struct ROSMsg smsg,rmsg;

  int iqoff=0; /* Sequence offset in bytes for current sequence relative to start of samples buffer*/
  int iqsze=0; /* Total number of bytes so far recorded into samples buffer*/

  int nave=0;

  int atstp=0.;
  int thr=0,lmt=0;
  int aflg=0,abflg=0;

  FILE *ftest=NULL;
  char test_file[255];
  char raw_file[255],data_file[255],strtemp[255];
  int temp;
  struct timespec time_now;

  void *dest=NULL; /*AJ*/
  int total_samples=0; /*AJ*/
  int usecs;
  short I,Q;
  double phi_m,phi_i,phi_d;
  int32 temp32;
  /* phase code declarations */
  int n,nsamp, *code,   Iout, Qout;
  uint32 uI32,uQ32;
  if (debug) {
    fprintf(stderr,"%s SiteIntegrate: start\n",station);
  }
  SiteTimExit(0);
  clock_gettime(CLOCK_REALTIME, &time_now);
  ttime=time_now.tv_sec;
  gmtime_r(&ttime,&tstruct);

  test_file[0]='\0';
  strcat(test_file,"/collect.now");
  strcat(test_file,channame);
  ftest=fopen(test_file, "r");

  if(ftest!=NULL){
    /*printf("\nSave decoded\n");*/
    fclose(ftest);
    ftest=NULL;
    data_file[0]='\0';
    /* data file directory*/
    strcat(data_file, "/data/diagnostic_samples/");
    /* data file year*/
    temp=(int)tstruct.tm_year+1900;
    sprintf(strtemp,"%d",temp);
    strcat(data_file, strtemp);
    /* data file month*/
    temp=(int)tstruct.tm_mon;
    sprintf(strtemp,"%d",temp+1);
    if(temp<10) strcat(data_file, "0");
    strcat(data_file, strtemp);
    /* data file day*/
    temp=(int)tstruct.tm_mday;
    sprintf(strtemp,"%d",temp);
    if(temp<10) strcat(data_file, "0");
    strcat(data_file, strtemp);
    /* data file hour*/
    temp=(int)tstruct.tm_hour;
    sprintf(strtemp,"%d",temp);
    if(temp<10) strcat(data_file, "0");
    strcat(data_file, strtemp);
    /* data file tens of minutes*/
    temp=(int)tstruct.tm_min;
    temp=(int)(temp/10);
    sprintf(strtemp,"%d",temp);
    strcat(data_file, strtemp);
    if(temp<10) strcat(data_file, "0");
    /* data file suffix*/
    strcat(data_file, ".");
    temp=rnum;
    sprintf(strtemp,"%d",temp);
    strcat(data_file, strtemp);
    sprintf(raw_file,"%s",data_file);
    strcat(data_file, ".diagnostic.txt");
    strcat(raw_file, ".raw.txt");
    strcat(data_file, channame);
    strcat(raw_file, channame);
    f_diagnostic_ascii=fopen(data_file, "a+");
    fprintf(stdout,"Filename: %s  Filepointer: %p\n",data_file,f_diagnostic_ascii);
  }
  if(f_diagnostic_ascii!=NULL) {
      fprintf(f_diagnostic_ascii,"SiteIntegrate: START %s",asctime(&tstruct));
      fprintf(f_diagnostic_ascii,"  bmnum=%8d nbaud=%8d txpl=%8d tfreq=%8d\n", bmnum, nbaud, txpl,tfreq);
      fprintf(f_diagnostic_ascii,"  mpinc=%8d mppul=%8d ptab= ", tsgprm.mpinc,tsgprm.mppul);
      for (i=0;i<tsgprm.mppul;i++) fprintf(f_diagnostic_ascii," %8d, ",tsgprm.pat[i]);
      fprintf(f_diagnostic_ascii,"\n");
  }

  if (nrang>=MAX_RANGE) return -1;
  for (j=0;j<LAG_SIZE;j++) lagsum[j]=0;

  if (mplgexs==0) {
    lagtable[0]=malloc(sizeof(int)*(mplgs+1));
    if(lagtable[0]==NULL) {
      fprintf(stderr,"Lagtable-0 is Null\n");
      SiteTimExit(-1);
    }
    lagtable[1]=malloc(sizeof(int)*(mplgs+1));
    if(lagtable[1]==NULL) {
      fprintf(stderr,"Lagtable-1 is Null\n");
      SiteTimExit(-1);
    }
    for (i=0;i<=mplgs;i++) {
      lagtable[0][i]=lags[i][0];
      lagtable[1][i]=lags[i][1];
    }
  } else {
    lagtable[0]=malloc(sizeof(int)*(mplgexs+1));
    if(lagtable[0]==NULL) {
      fprintf(stderr,"Lagtable-0 is Null\n");
      SiteTimExit(-1);
    }
    lagtable[1]=malloc(sizeof(int)*(mplgexs+1));
    if(lagtable[1]==NULL) {
      fprintf(stderr,"Lagtable-1 is Null\n");
      SiteTimExit(-1);
    }

    for (i=0;i<=mplgexs;i++) {
      lagtable[0][i]=lags[i][0];
      lagtable[1][i]=lags[i][1];
      j=abs(lags[i][0]-lags[i][1]);
      lagsum[j]++;
    }
  }


  total_samples=tsgprm.samples+tsgprm.smdelay;
  smpnum=total_samples;
  skpnum=tsgprm.smdelay;  /*skpnum != 0  returns 1, which is used as the dflg argument in ACFCalculate to enable smdelay usage in offset calculations*/
  badrng=ACFBadLagZero(&tsgprm,mplgs,lagtable);

  gettimeofday(&tick,NULL);
  gettimeofday(&tack,NULL);

  for (i=0;i<MAX_RANGE;i++) {
      pwr0[i]=0;
      for (j=0;j<LAG_SIZE*2;j++) {
        acfd[i*LAG_SIZE*2+j]=0;
        xcfd[i*LAG_SIZE*2+j]=0;
      }
  }

/* Seq loop to trigger and collect data */
  while (1) {
    SiteTimExit(0);
    if(f_diagnostic_ascii!=NULL) {
      clock_gettime(CLOCK_REALTIME, &time_now);
      ttime=time_now.tv_sec;
      gmtime_r(&ttime,&tstruct);
      fprintf(f_diagnostic_ascii,"Sequence: START: %8d\n",nave);
      fprintf(f_diagnostic_ascii,"  sec: %8d nsec: %12ld\n",(int)time_now.tv_sec,time_now.tv_nsec);
    }

    seqtval[nave].tv_sec=tick.tv_sec;
    seqtval[nave].tv_nsec=tick.tv_usec*1000;
    seqatten[nave]=0.;
    seqnoise[nave]=0;
    seqbadtr[nave].num=0;
  

    tval=(tick.tv_sec+tick.tv_usec/1.0e6)-
         (tack.tv_sec+tack.tv_usec/1.0e6);

    if (nave>0) tavg=tval/nave; 
     
    tick.tv_sec+=floor(tavg);
    tick.tv_usec+=1.0e6*(tavg-floor(tavg));

    time_diff=(tick.tv_sec-tock.tv_sec);
    time_diff+=(tick.tv_usec-tock.tv_usec)/1E6;
    /* Tests to break out of Integration loop */
    if (tock.tv_sec+tock.tv_usec==0) {
      /*Integration not requested, break when nave > 0 */
      if (nave > 0) break; 
    } else {
      /*Integration requested, break when elapsed time is greater than integration scan */
      if (time_diff > 0.0) {
        break;
      }
    }


    rprm.tbeam=bmnum;   
    rprm.tfreq=tfreq;   
    if (rfreq < 0) { 
      rprm.rfreq=tfreq;   
    } else {
      rprm.rfreq=rfreq;   
    }
    rprm.trise=5000;   
    rprm.baseband_samplerate=((double)nbaud/(double)txpl)*1E6; 
    rprm.filter_bandwidth=rprm.baseband_samplerate; 
    rprm.match_filter=dmatch;
    rprm.number_of_samples=total_samples+nbaud+10; 
    rprm.priority=cnum;
    rprm.buffer_index=0;  

    usecs=(int)(rprm.number_of_samples/rprm.baseband_samplerate*1E6);
    if (debug) {
      fprintf(stderr,"%s SiteIntegrate: rfreq %d tfreq %d\n",station,rprm.rfreq,rprm.tfreq);
    }

    smsg.type=SET_PARAMETERS;
    TCPIPMsgSend(sock,&smsg,sizeof(struct ROSMsg));
    TCPIPMsgSend(sock,&rprm,sizeof(struct ControlPRM));
    TCPIPMsgRecv(sock,&rmsg,sizeof(struct ROSMsg));
    if (debug) {
      fprintf(stderr,"SET_PARAMETERS:type=%c\n",rmsg.type);
      fprintf(stderr,"SET_PARAMETERS:status=%d\n",rmsg.status);
    }



    smsg.type=SET_READY_FLAG;
    TCPIPMsgSend(sock,&smsg,sizeof(struct ROSMsg));
    TCPIPMsgRecv(sock,&rmsg,sizeof(struct ROSMsg));
    if (debug) {
      fprintf(stderr,"SET_READY_FLAG:type=%c\n",rmsg.type);
      fprintf(stderr,"SET_READY_FLAG:status=%d\n",rmsg.status);
    }

usleep(usecs);

/*  FIXME: This is not defined 
    smsg.type=WAIT_FOR_DATA;
    TCPIPMsgSend(sock,&smsg,sizeof(struct ROSMsg));
    TCPIPMsgRecv(sock,&rmsg,sizeof(struct ROSMsg));
    if (debug) {
      fprintf(stderr,"SET_READY_FLAG:type=%c\n",rmsg.type);
      fprintf(stderr,"SET_READY_FLAG:status=%d\n",rmsg.status);
    }
*/

    smsg.type=GET_DATA;
    if (rdata.main!=NULL) free(rdata.main);
    if (rdata.back!=NULL) free(rdata.back);
    rdata.main=NULL;
    rdata.back=NULL;
    TCPIPMsgSend(sock,&smsg,sizeof(struct ROSMsg));
    if (debug) {
      fprintf(stderr,"%s GET_DATA: recv dprm\n",station);
    }
    TCPIPMsgRecv(sock,&dprm,sizeof(struct DataPRM));
    if(rdata.main) free(rdata.main);
    if(rdata.back) free(rdata.back);
    if (debug) 
        fprintf(stderr,"%s GET_DATA: samples %d status %d\n",station,dprm.samples,dprm.status);
      
    if(dprm.status==0) {
      if (debug) {
        fprintf(stderr,"%s GET_DATA: rdata.main: uint32: %ld array: %ld\n",station,sizeof(uint32),sizeof(uint32)*dprm.samples);
      }
      rdata.main=malloc(sizeof(uint32)*dprm.samples);
      rdata.back=malloc(sizeof(uint32)*dprm.samples);
      if (debug) {
        fprintf(stderr,"%s GET_DATA: recv main\n",station);
      }
      TCPIPMsgRecv(sock, rdata.main, sizeof(uint32)*dprm.samples);
      if (debug) {
        fprintf(stderr,"%s GET_DATA: recv back\n",station);
      }
      TCPIPMsgRecv(sock, rdata.back, sizeof(uint32)*dprm.samples);

      if (badtrdat.start_usec !=NULL) free(badtrdat.start_usec);
      if (badtrdat.duration_usec !=NULL) free(badtrdat.duration_usec);
      badtrdat.start_usec=NULL;
      badtrdat.duration_usec=NULL;
      if (debug) {
        fprintf(stderr,"%s GET_DATA: trtimes length %d\n",station,badtrdat.length);
      }
      TCPIPMsgRecv(sock, &badtrdat.length, sizeof(badtrdat.length));
      if (debug) 
        fprintf(stderr,"%s GET_DATA: badtrdat.start_usec: uint32: %ld array: %ld\n",station,sizeof(uint32),sizeof(uint32)*badtrdat.length);
      badtrdat.start_usec=malloc(sizeof(uint32)*badtrdat.length);
      badtrdat.duration_usec=malloc(sizeof(uint32)*badtrdat.length);
      if (debug) {
        fprintf(stderr,"%s GET_DATA: start_usec\n",station);
      }
      TCPIPMsgRecv(sock, badtrdat.start_usec,
                 sizeof(uint32)*badtrdat.length);
      if (debug) {
        fprintf(stderr,"%s GET_DATA: duration_usec\n",station);
      }
      TCPIPMsgRecv(sock, badtrdat.duration_usec,
                 sizeof(uint32)*badtrdat.length);
      TCPIPMsgRecv(sock, &num_transmitters, sizeof(int));
      TCPIPMsgRecv(sock, &txstatus.AGC, sizeof(int)*num_transmitters);
      TCPIPMsgRecv(sock, &txstatus.LOWPWR, sizeof(int)*num_transmitters);
    }
    TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
    if (debug) {
      fprintf(stderr,"%s GET_DATA:type=%c\n",station,rmsg.type);
      fprintf(stderr,"%s GET_DATA:status=%d\n",station,rmsg.status);
    }
    smsg.type=GET_PARAMETERS;
    TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
    TCPIPMsgRecv(sock, &rprm, sizeof(struct ControlPRM));
    TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
    if (debug) {
      fprintf(stderr,"%s GET_PARAMETERS:type=%c\n",station,rmsg.type);
      fprintf(stderr,"%s GET_PARAMETERS:status=%d\n",station,rmsg.status);
      fprintf(stderr,"%s Number of samples: dprm.samples:%d tsprm.samples:%d total_samples:%d\n",station,dprm.samples,tsgprm.samples,total_samples);
      fprintf(stderr,"%s nave=%d\n",station,nave);
      fprintf(stderr,"%s dprm.status=%d\n",station,dprm.status);
    }
    ttime=dprm.event_secs;
    if ( ttime < 100 ) {
      ttime=time_now.tv_sec;
    }
    gmtime_r(&ttime,&tstruct);
    if(seqlog_dir!=NULL) { 
      sprintf(filename,"%s/seqlog.%s%s.%04d%02d%02d",seqlog_dir,station,channame,tstruct.tm_year+1900,tstruct.tm_mon+1,tstruct.tm_mday);
      if(strcmp(filename,seqlog_name)!=0) {
        strcpy(seqlog_name,filename);
        if (seqlog!=NULL) {
          fflush(seqlog);
          fclose(seqlog);
          seqlog=NULL;
        }
        fprintf(stdout,"seqlog filename: %s\n",seqlog_name);
        seqlog=fopen(seqlog_name,"a+");
        fflush(stdout);
      }
    }
    if (seqlog!=NULL) {
      fwrite(&dprm.event_secs,sizeof(int32),1,seqlog);
      temp32=floor(dprm.event_nsecs/1000); 
      fwrite(&temp32,sizeof(int32),1,seqlog);
      fwrite(&rprm.tbeam,sizeof(int32),1,seqlog);
      fwrite(&rprm.tfreq,sizeof(int32),1,seqlog);
      fwrite(&badtrdat.length,sizeof(int32),1,seqlog);
      for(i=0;i<badtrdat.length;i++) {
        temp32=badtrdat.start_usec[i]; 
        fwrite(&temp32,sizeof(int32),1,seqlog);
        temp32=badtrdat.duration_usec[i]; 
        fwrite(&temp32,sizeof(int32),1,seqlog);
      }
    }
    if(f_diagnostic_ascii!=NULL) {
        fprintf(f_diagnostic_ascii,"** TX: ");
        for(i=0;i<num_transmitters;i++)
          fprintf(f_diagnostic_ascii,"%3d ",i);
        fprintf(f_diagnostic_ascii,"\n");
        fprintf(f_diagnostic_ascii,"  AGC: ");
        for(i=0;i<num_transmitters;i++)
          fprintf(f_diagnostic_ascii,"%3d ",( 1 ^ txstatus.AGC[i]));
        fprintf(f_diagnostic_ascii,"\n");
        fprintf(f_diagnostic_ascii,"  LOW: ");
        for(i=0;i<num_transmitters;i++)
          fprintf(f_diagnostic_ascii,"%3d ",txstatus.LOWPWR[i]);
        fprintf(f_diagnostic_ascii,"\n");
    }


    if(nave==0) {
      bmnum=rprm.tbeam;
      tfreq=rprm.tfreq;
    }
    if (rprm.tbeam != bmnum) {
      fprintf(stderr,"New beam :: end integration\n");
      fflush(stderr);
      break;
    } else {
    }

/* JDS : For testing only */
/*
    dprm.status=0;
    dprm.samples=0;
    if (rdata.main!=NULL) free(rdata.main);
    if (rdata.back!=NULL) free(rdata.back);
    rdata.main=NULL;
    rdata.back=NULL;
    rdata.main=malloc(sizeof(uint32)*dprm.samples);
    rdata.back=malloc(sizeof(uint32)*dprm.samples);
    badtrdat.length=0;
*/
/* JDS: End testing block */
    code=pcode;
    if(f_diagnostic_ascii!=NULL) {
      fprintf(f_diagnostic_ascii,"Sequence: Parameters: START\n");
      fprintf(f_diagnostic_ascii,"  bmnum=%8d nbaud=%8d txpl=%8d tfreq=%8d code=", bmnum, nbaud, txpl,tfreq);
      for(i=0;i<nbaud;i++){
        if (code!=NULL)
          fprintf(f_diagnostic_ascii,"%8d,", code[i]);
        else
          fprintf(f_diagnostic_ascii,"%8d,", 1);
      }
      fprintf(f_diagnostic_ascii,"\n");
      fprintf(f_diagnostic_ascii,"Sequence: Parameters: END\n");
      fprintf(f_diagnostic_ascii,"Sequence: Invert %d\n",invert);
    }

    if(dprm.status==0) {
      nsamp=(int)dprm.samples;
     
      if(invert!=0) {
        for(n=0;n<nsamp;n++){
          Q=(short)((rdata.main[n] & 0xffff0000) >> 16);
          I=(short)(rdata.main[n] & 0x0000ffff);
          Q=-Q;
          I=-I;
          uQ32=((uint32) Q) << 16;
          uI32=((uint32) I) & 0xFFFF;
          (rdata.main)[n]=uQ32|uI32;
        }
      }
/*
      fp=f_diagnostic_ascii;
      f_diagnostic_ascii=stderr;
*/
      if(f_diagnostic_ascii!=NULL) {
        fprintf(f_diagnostic_ascii,"Sequence : Raw Data : START\n");
        fprintf(f_diagnostic_ascii,"  nsamp: %8d\n",nsamp);
        fprintf(f_diagnostic_ascii,"index I_m Q_m I_m^2+Q_m^2 phi_m I_i Q_i I_i^2+Q_i^2 phi_i phi_d\n");
        for(n=0;n<nsamp;n++){
          Q=(short)((rdata.main[n] & 0xffff0000) >> 16);
          I=(short)(rdata.main[n] & 0x0000ffff);
          phi_m=atan2(Q,I);
          if(f_diagnostic_ascii!=NULL) {
            fprintf(f_diagnostic_ascii,"%8d %8d %8d %8d %8.3lf ", n, I, Q, (int)(I*I+Q*Q), phi_m);
           }
          Q=(short)((rdata.back[n] & 0xffff0000) >> 16);
          I=(short)(rdata.back[n] & 0x0000ffff);
          phi_i=atan2(Q,I);
          phi_d=phi_i-phi_m;
          if(phi_d >=  M_PI ) phi_d=phi_d-(2.*M_PI);
          if(phi_d < -M_PI ) phi_d=phi_d+(2.*M_PI);
          if(f_diagnostic_ascii!=NULL) {
            fprintf(f_diagnostic_ascii,"%8d %8d %8d %8.3lf %8.3lf\n", I, Q, (int)(I*I+Q*Q),phi_i,phi_d);
          }
        }
        fprintf(f_diagnostic_ascii,"Sequence: Raw Data: END\n");
      }
/*
      f_diagnostic_ascii=fp;
*/
    /* decode phase coding here */
      if(nbaud>1){
        if(f_diagnostic_ascii!=NULL) {
            fprintf(f_diagnostic_ascii,"PCODE: DECODE_START\n");
            fprintf(f_diagnostic_ascii,"nsamp: %8d\n",nsamp);
        }

        nsamp=(int)dprm.samples;
        code=pcode;
        for(n=0;n<(nsamp-nbaud);n++){
          Iout=0;
          Qout=0;
          for(i=0;i<nbaud;i++){
            Q=((rdata.main)[n+i] & 0xffff0000) >> 16;
            I=(rdata.main)[n+i] & 0x0000ffff;
            Iout+=(int)I*(int)code[i];
            Qout+=(int)Q*(int)code[i];
          }

          Iout/=nbaud;
          Qout/=nbaud;
          I=(short)Iout;
          Q=(short)Qout;

          if(f_diagnostic_ascii!=NULL) {
            fprintf(f_diagnostic_ascii,"%8d %8d %8d %8d ", n, I, Q, (int)sqrt(I*I+Q*Q));
          }
          uQ32=((uint32) Q) << 16;
          uI32=((uint32) I) & 0xFFFF;
          (rdata.main)[n]=uQ32|uI32;
                

          Iout=0;
          Qout=0;
          for(i=0;i<nbaud;i++){
            Q=((rdata.back)[n+i] & 0xffff0000) >> 16;
            I=(rdata.back)[n+i] & 0x0000ffff;
            Iout+=(int)I*(int)code[i];
            Qout+=(int)Q*(int)code[i];
          }
          Iout/=nbaud;
          Qout/=nbaud;
          I=(short)Iout;
          Q=(short)Qout;
          if(f_diagnostic_ascii!=NULL) {
            fprintf(f_diagnostic_ascii,"%8d %8d %8d\n", I, Q, (int)sqrt(I*I+Q*Q));
          }
          uQ32=((uint32) Q) << 16;
          uI32=((uint32) I) & 0xFFFF;
          (rdata.back)[n]=uQ32|uI32;
        }
        if(f_diagnostic_ascii!=NULL) fprintf(f_diagnostic_ascii,"PCODE: DECODE_END\n");

      } else {
      }
/*
      if(dprm.samples<total_samples) {
        fprintf(stderr,"Not enough  samples from the ROS in SiteIntegrate\n");
        fflush(stderr);
      }
*/
    /* copy samples here */

      seqoff[nave]=iqsze/2;/*Sequence offset in 16bit units */
      seqsze[nave]=dprm.samples*2*2; /* Sequence length in 16bit units */

      if(seqbadtr[nave].start!=NULL)  free(seqbadtr[nave].start);
      if(seqbadtr[nave].length!=NULL) free(seqbadtr[nave].length);
      seqbadtr[nave].start=NULL;
      seqbadtr[nave].length=NULL;
      seqbadtr[nave].num=badtrdat.length;
      seqbadtr[nave].start=malloc(sizeof(uint32)*badtrdat.length);
      seqbadtr[nave].length=malloc(sizeof(uint32)*badtrdat.length);

      memcpy(seqbadtr[nave].start,badtrdat.start_usec,
           sizeof(uint32)*badtrdat.length);
      memcpy(seqbadtr[nave].length,badtrdat.duration_usec,
           sizeof(uint32)*badtrdat.length);

/* AJ new way, does work for some reason */
/* samples is natively an int16 pointer */
/* rdata.main is natively an uint32 pointer */
/* rdata.back is natively an uint32 pointer */
/* total_samples*8 represents number of bytes for main and back samples */

      
      dest = (void *)(samples);  /* look iqoff bytes into samples area */
      dest+=iqoff;
      if ((iqoff+dprm.samples*2*sizeof(uint32) )<iqbufsize) {
        memmove(dest,rdata.main,dprm.samples*sizeof(uint32));
        dest += dprm.samples*sizeof(uint32); /* skip ahead number of samples * 32 bit per sample to account for rdata.main*/
        memmove(dest,rdata.back,dprm.samples*sizeof(uint32));
      } else {
        fprintf(stderr,"IQ Buffer overrun in SiteIntegrate\n");
        fflush(stderr);
      }
      iqsze+=dprm.samples*sizeof(uint32)*2;  /*  Total of number bytes so far copied into samples array */
      if (debug) {
        fprintf(stderr,"%s seq %d :: ioff: %8d\n",station,nave,iqoff);
        fprintf(stderr,"%s seq %d :: rdata.main 16bit :\n",station,nave);
        fprintf(stderr," [  n  ] :: [  Im  ] [  Qm  ] :: [ Ii ] [ Qi ]\n");
        nsamp=(int)dprm.samples;
        for(n=0;n<(nsamp);n++){
          Q=((rdata.main)[n] & 0xffff0000) >> 16;
          I=(rdata.main)[n] & 0x0000ffff;
          fprintf(stderr," %7d :: %7d %7d ",n,(int)I,(int)Q);
          Q=((rdata.back)[n] & 0xffff0000) >> 16;
          I=(rdata.back)[n] & 0x0000ffff;
          fprintf(stderr,":: %7d %7d\n",(int)I,(int)Q);
        }
        dest = (void *)(samples);
        dest += iqoff;
        fprintf(stderr,"%s seq %d :: rdata.back 16bit 30: %8d %8d\n",station,nave,
           ((int16 *)rdata.back)[60],((int16 *)rdata.back)[61]);
        dest += dprm.samples*sizeof(uint32);
        fprintf(stderr,"%s seq %d :: samples    16bit 30: %8d %8d\n",station,nave,
           ((int16 *)dest)[60],((int16 *)dest)[61]);
        fprintf(stderr,"%s seq %d :: iqsze: %8d\n",station,nave,iqsze);
      }

    /* calculate ACF */   
      if (mplgexs==0) {
        dest = (void *)(samples);
        dest += iqoff;
        rngoff=2*rxchn; 
        if (debug) 
        fprintf(stderr,"%s seq %d :: rngoff %d rxchn %d\n",station,nave,rngoff,rxchn);
        if (debug) 
        fprintf(stderr,"%s seq %d :: ACFSumPower\n",station,nave);
        aflg=ACFSumPower(&tsgprm,mplgs,lagtable,pwr0,
		     (int16 *) dest,rngoff,skpnum!=0,
                     roff,ioff,badrng,
                     noise,mxpwr,seqatten[nave]*atstp,
                     thr,lmt,&abflg);
        if (debug) 
        fprintf(stderr,"%s seq %d :: rngoff %d rxchn %d\n",station,nave,rngoff,rxchn);
        if (debug) 
        fprintf(stderr,"%s seq %d :: ACFCalculate acf\n",station,nave);
        ACFCalculate(&tsgprm,(int16 *) dest,rngoff,skpnum!=0,
          roff,ioff,mplgs,lagtable,acfd,ACF_PART,2*dprm.samples,badrng,seqatten[nave]*atstp,NULL);
        if (xcf ==1 ){
          if (debug) 
            fprintf(stderr,"%s seq %d :: rngoff %d rxchn %d\n",station,nave,rngoff,rxchn);
          if (debug) 
            fprintf(stderr,"%s seq %d :: ACFCalculate xcf\n",station,nave);
          ACFCalculate(&tsgprm,(int16 *) dest,rngoff,skpnum!=0,
                    roff,ioff,mplgs,lagtable,xcfd,XCF_PART,2*dprm.samples,badrng,seqatten[nave]*atstp,NULL);
        }
        if ((nave>0) && (seqatten[nave] !=seqatten[nave])) {
        if (debug) 
        fprintf(stderr,"%s seq %d :: rngoff %d rxchn %d\n",station,nave,rngoff,rxchn);
        if (debug) 
          fprintf(stderr,"%s seq %d :: ACFNormalize\n",station,nave);
              ACFNormalize(pwr0,acfd,xcfd,tsgprm.nrang,mplgs,atstp); 
        }  
        if (debug) 
        fprintf(stderr,"%s seq %d :: rngoff %d rxchn %d\n",station,nave,rngoff,rxchn);


      }
      nave++;
      iqoff=iqsze;  /* set the offset bytes for the next sequence */

    } else {
    }
    gettimeofday(&tick,NULL);
    if(f_diagnostic_ascii!=NULL) fprintf(f_diagnostic_ascii,"Sequence: END\n");


  }
  if(seqlog!=NULL) fflush(seqlog);

  /* Now divide by nave to get the average pwr0 and acfd values for the 
     integration period */ 

   if (mplgexs==0) {

     if (nave > 0 ) {
       ACFAverage(pwr0,acfd,xcfd,nave,tsgprm.nrang,mplgs);
/*
       for(range=0; range < nrang;range++) {
         pwr0[range]=(double)pwr0[range]/(double)nave;

         for(lag=0;lag < mplgs; lag++) {     
           acfd[range*(2*mplgs)+2*lag]= (double) acfd[range*(2*mplgs)+2*lag]/
                                       (double) nave;
           acfd[range*(2*mplgs)+2*lag+1]= (double) acfd[range*(2*mplgs)+2*lag+1]/
                                       (double) nave;
         }
       }
*/
     }
   } else if (nave>0) {
     /* ACFEX calculation */
     ACFexCalculate(&tsgprm,(int16 *) samples,nave*smpnum,nave,smpnum,
                   roff,ioff,
                   mplgs,mplgexs,lagtable,lagsum,
                   pwr0,acfd,&noise);
   }
   free(lagtable[0]);
   free(lagtable[1]);
   if (debug) {
     fprintf(stderr,"%s SiteIntegrate: iqsize in bytes: %ld in 16bit samples:  %ld in 32bit samples: %ld\n",station,(long int)iqsze,(long int)iqsze/2,(long int)iqsze/4);
     fprintf(stderr,"%s SiteIntegrate: end: nave: %d\n",station,nave);
   }
   if(f_diagnostic_ascii!=NULL) fprintf(f_diagnostic_ascii,"SiteIntegrate: END\n");
   if(f_diagnostic_ascii!=NULL){
     fclose(f_diagnostic_ascii);
     f_diagnostic_ascii=NULL;
   }

   SiteTimExit(0);
   return nave;
}

int SiteTimEndScan(int bsc,int bus) {

  struct ROSMsg smsg,rmsg;
  
  struct timeval tock;
  struct timeval tick;
  double bnd;
  double tme;
  int count=0;
  SiteTimExit(0);
  bnd=bsc+bus/USEC;
  if (debug) {
    fprintf(stderr,"SiteTimEndScan: start\n");
  }

  if (gettimeofday(&tock,NULL)==-1) return -1;

  tme=tock.tv_sec+tock.tv_usec/USEC;
  tme=bnd*floor(1.0+tme/bnd);
  tock.tv_sec=tme;
  tock.tv_usec=(tme-floor(tme))*USEC;

  smsg.type=SET_INACTIVE;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));

  gettimeofday(&tick,NULL);
  while (1) {
    SiteTimExit(0);
    if (tick.tv_sec>tock.tv_sec) break;
    if ((tick.tv_sec==tock.tv_sec) && (tick.tv_usec>tock.tv_usec)) break;
    smsg.type=PING;
    TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
    TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));

    if (debug) {
      fprintf(stderr,"PING:type=%c\n",rmsg.type);
      fprintf(stderr,"PING:status=%d\n",rmsg.status);
      fprintf(stderr,"PING:count=%d\n",count);
      fflush(stderr);
    }
    count++;
    SiteTimExit(0);
    usleep(50000);
    SiteTimExit(0);
    gettimeofday(&tick,NULL);
  }
/*
  smsg.type=SET_ACTIVE;
  TCPIPMsgSend(sock, &smsg, sizeof(struct ROSMsg));
  TCPIPMsgRecv(sock, &rmsg, sizeof(struct ROSMsg));
*/
  if (debug) {
    fprintf(stderr,"SiteTimEndScan: end\n");
  }
  return 0;
}







