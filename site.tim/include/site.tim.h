/* site.standard.h
   ==========
*/


#ifndef _SITETIM_H
#define _SITETIM_H

int SiteTimStart(char *host,char *ststr);
int SiteTimKeepAlive();
int SiteTimSetupRadar();
int SiteTimStartScan();
int SiteTimStartIntt(int intsc,int intus);
int SiteTimFCLR(int stfreq,int edfreq);
int SiteTimTimeSeq(int *ptab);
int SiteTimIntegrate(int (*lags)[2]);
int SiteTimEndScan(int bsc,int bus);
void SiteTimExit(int signum);

#endif

