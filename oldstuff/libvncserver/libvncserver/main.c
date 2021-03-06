/*
 *  This file is called main.c, because it contains most of the new functions
 *  for use with LibVNCServer.
 *
 *  LibVNCServer (C) 2001 Johannes E. Schindelin <Johannes.Schindelin@gmx.de>
 *  Original OSXvnc (C) 2001 Dan McGuirk <mcguirk@incompleteness.net>.
 *  Original Xvnc (C) 1999 AT&T Laboratories Cambridge.  
 *  All Rights Reserved.
 *
 *  see GPL (latest version) for full details
 */

#include <rfb/rfb.h>
#include <rfb/rfbregion.h>

#include <stdarg.h>
#include <errno.h>

#ifndef false
#define false 0
#define true -1
#endif

#ifdef LIBVNCSERVER_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <signal.h>
#include <time.h>

#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
MUTEX(logMutex);
#endif

int rfbEnableLogging=1;

#ifdef LIBVNCSERVER_WORDS_BIGENDIAN
char rfbEndianTest = 0;
#else
char rfbEndianTest = -1;
#endif

/* from rfbserver.c */
void rfbIncrClientRef(rfbClientPtr cl);
void rfbDecrClientRef(rfbClientPtr cl);

void rfbLogEnable(int enabled) {
  rfbEnableLogging=enabled;
}

/*
 * rfbLog prints a time-stamped message to the log file (stderr).
 */

void
rfbDefaultLog(const char *format, ...)
{
    va_list args;
    char buf[256];
    time_t log_clock;

    if(!rfbEnableLogging)
      return;

    LOCK(logMutex);
    va_start(args, format);

    time(&log_clock);
    strftime(buf, 255, "%d/%m/%Y %X ", localtime(&log_clock));
    fprintf(stderr,buf);

    vfprintf(stderr, format, args);
    fflush(stderr);

    va_end(args);
    UNLOCK(logMutex);
}

rfbLogProc rfbLog=rfbDefaultLog;
rfbLogProc rfbErr=rfbDefaultLog;

void rfbLogPerror(const char *str)
{
    rfbErr("%s: %s\n", str, strerror(errno));
}

void rfbScheduleCopyRegion(rfbScreenInfoPtr rfbScreen,sraRegionPtr copyRegion,int dx,int dy)
{  
   rfbClientIteratorPtr iterator;
   rfbClientPtr cl;

   rfbUndrawCursor(rfbScreen);
  
   iterator=rfbGetClientIterator(rfbScreen);
   while((cl=rfbClientIteratorNext(iterator))) {
     LOCK(cl->updateMutex);
     if(cl->useCopyRect) {
       sraRegionPtr modifiedRegionBackup;
       if(!sraRgnEmpty(cl->copyRegion)) {
	  if(cl->copyDX!=dx || cl->copyDY!=dy) {
	     /* if a copyRegion was not yet executed, treat it as a
	      * modifiedRegion. The idea: in this case it could be
	      * source of the new copyRect or modified anyway. */
	     sraRgnOr(cl->modifiedRegion,cl->copyRegion);
	     sraRgnMakeEmpty(cl->copyRegion);
	  } else {
	     /* we have to set the intersection of the source of the copy
	      * and the old copy to modified. */
	     modifiedRegionBackup=sraRgnCreateRgn(copyRegion);
	     sraRgnOffset(modifiedRegionBackup,-dx,-dy);
	     sraRgnAnd(modifiedRegionBackup,cl->copyRegion);
	     sraRgnOr(cl->modifiedRegion,modifiedRegionBackup);
	     sraRgnDestroy(modifiedRegionBackup);
	  }
       }
	  
       sraRgnOr(cl->copyRegion,copyRegion);
       cl->copyDX = dx;
       cl->copyDY = dy;

       /* if there were modified regions, which are now copied,
	* mark them as modified, because the source of these can be overlapped
	* either by new modified or now copied regions. */
       modifiedRegionBackup=sraRgnCreateRgn(cl->modifiedRegion);
       sraRgnOffset(modifiedRegionBackup,dx,dy);
       sraRgnAnd(modifiedRegionBackup,cl->copyRegion);
       sraRgnOr(cl->modifiedRegion,modifiedRegionBackup);
       sraRgnDestroy(modifiedRegionBackup);

#if 0
       /* TODO: is this needed? Or does it mess up deferring? */
       /* while(!sraRgnEmpty(cl->copyRegion)) */ {
#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
	 if(!cl->screen->backgroundLoop)
#endif
	   {
	     sraRegionPtr updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
	     sraRgnOr(updateRegion,cl->copyRegion);
	     UNLOCK(cl->updateMutex);
	     rfbSendFramebufferUpdate(cl,updateRegion);
	     sraRgnDestroy(updateRegion);
	     continue;
	   }
       }
#endif
     } else {
       sraRgnOr(cl->modifiedRegion,copyRegion);
     }
     TSIGNAL(cl->updateCond);
     UNLOCK(cl->updateMutex);
   }

   rfbReleaseClientIterator(iterator);
}

void rfbDoCopyRegion(rfbScreenInfoPtr screen,sraRegionPtr copyRegion,int dx,int dy)
{
   sraRectangleIterator* i;
   sraRect rect;
   int j,widthInBytes,bpp=screen->serverFormat.bitsPerPixel/8,
    rowstride=screen->paddedWidthInBytes;
   char *in,*out;

   rfbUndrawCursor(screen);
  
   /* copy it, really */
   i = sraRgnGetReverseIterator(copyRegion,dx<0,dy<0);
   while(sraRgnIteratorNext(i,&rect)) {
     widthInBytes = (rect.x2-rect.x1)*bpp;
     out = screen->frameBuffer+rect.x1*bpp+rect.y1*rowstride;
     in = screen->frameBuffer+(rect.x1-dx)*bpp+(rect.y1-dy)*rowstride;
     if(dy<0)
       for(j=rect.y1;j<rect.y2;j++,out+=rowstride,in+=rowstride)
	 memmove(out,in,widthInBytes);
     else {
       out += rowstride*(rect.y2-rect.y1-1);
       in += rowstride*(rect.y2-rect.y1-1);
       for(j=rect.y2-1;j>=rect.y1;j--,out-=rowstride,in-=rowstride)
	 memmove(out,in,widthInBytes);
     }
   }
  
   rfbScheduleCopyRegion(screen,copyRegion,dx,dy);
}

void rfbDoCopyRect(rfbScreenInfoPtr screen,int x1,int y1,int x2,int y2,int dx,int dy)
{
  sraRegionPtr region = sraRgnCreateRect(x1,y1,x2,y2);
  rfbDoCopyRegion(screen,region,dx,dy);
}

void rfbScheduleCopyRect(rfbScreenInfoPtr screen,int x1,int y1,int x2,int y2,int dx,int dy)
{
  sraRegionPtr region = sraRgnCreateRect(x1,y1,x2,y2);
  rfbScheduleCopyRegion(screen,region,dx,dy);
}

void rfbMarkRegionAsModified(rfbScreenInfoPtr screen,sraRegionPtr modRegion)
{
   rfbClientIteratorPtr iterator;
   rfbClientPtr cl;

   iterator=rfbGetClientIterator(screen);
   while((cl=rfbClientIteratorNext(iterator))) {
     LOCK(cl->updateMutex);
     sraRgnOr(cl->modifiedRegion,modRegion);
     TSIGNAL(cl->updateCond);
     UNLOCK(cl->updateMutex);
   }

   rfbReleaseClientIterator(iterator);
}

void rfbMarkRectAsModified(rfbScreenInfoPtr screen,int x1,int y1,int x2,int y2)
{
   sraRegionPtr region;
   int i;

   if(x1>x2) { i=x1; x1=x2; x2=i; }
   if(x1<0) x1=0;
   if(x2>=screen->width) x2=screen->width-1;
   if(x1==x2) return;
   
   if(y1>y2) { i=y1; y1=y2; y2=i; }
   if(y1<0) y1=0;
   if(y2>=screen->height) y2=screen->height-1;
   if(y1==y2) return;
   
   region = sraRgnCreateRect(x1,y1,x2,y2);
   rfbMarkRegionAsModified(screen,region);
   sraRgnDestroy(region);
}

#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
static void *
clientOutput(void *data)
{
    rfbClientPtr cl = (rfbClientPtr)data;
    rfbBool haveUpdate;
    sraRegion* updateRegion;

    while (1) {
        haveUpdate = false;
        while (!haveUpdate) {
            if (cl->sock == -1) {
                /* Client has disconnected. */
                return NULL;
            }
	    LOCK(cl->updateMutex);
	    haveUpdate = FB_UPDATE_PENDING(cl);
	    if(!haveUpdate) {
		updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
		haveUpdate = sraRgnAnd(updateRegion,cl->requestedRegion);
		sraRgnDestroy(updateRegion);
	    }
	    UNLOCK(cl->updateMutex);

            if (!haveUpdate) {
                WAIT(cl->updateCond, cl->updateMutex);
		UNLOCK(cl->updateMutex); /* we really needn't lock now. */
            }
        }
        
        /* OK, now, to save bandwidth, wait a little while for more
           updates to come along. */
        usleep(cl->screen->deferUpdateTime * 1000);

        /* Now, get the region we're going to update, and remove
           it from cl->modifiedRegion _before_ we send the update.
           That way, if anything that overlaps the region we're sending
           is updated, we'll be sure to do another update later. */
        LOCK(cl->updateMutex);
	updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
        UNLOCK(cl->updateMutex);

        /* Now actually send the update. */
	rfbIncrClientRef(cl);
        rfbSendFramebufferUpdate(cl, updateRegion);
	rfbDecrClientRef(cl);

	sraRgnDestroy(updateRegion);
    }

    return NULL;
}

static void *
clientInput(void *data)
{
    rfbClientPtr cl = (rfbClientPtr)data;
    pthread_t output_thread;
    pthread_create(&output_thread, NULL, clientOutput, (void *)cl);

    while (1) {
        rfbProcessClientMessage(cl);
        if (cl->sock == -1) {
            /* Client has disconnected. */
            break;
        }
    }

    /* Get rid of the output thread. */
    LOCK(cl->updateMutex);
    TSIGNAL(cl->updateCond);
    UNLOCK(cl->updateMutex);
    IF_PTHREADS(pthread_join(output_thread, NULL));

    rfbClientConnectionGone(cl);

    return NULL;
}

static void*
listenerRun(void *data)
{
    rfbScreenInfoPtr screen=(rfbScreenInfoPtr)data;
    int client_fd;
    struct sockaddr_in peer;
    rfbClientPtr cl;
    size_t len;

    len = sizeof(peer);

    /* TODO: this thread wont die by restarting the server */
    while ((client_fd = accept(screen->listenSock, 
                               (struct sockaddr*)&peer, &len)) >= 0) {
        cl = rfbNewClient(screen,client_fd);
        len = sizeof(peer);

	if (cl && !cl->onHold )
		rfbStartOnHoldClient(cl);
    }
    return(NULL);
}

void 
rfbStartOnHoldClient(rfbClientPtr cl)
{
    pthread_create(&cl->client_thread, NULL, clientInput, (void *)cl);
}

#else

void 
rfbStartOnHoldClient(rfbClientPtr cl)
{
	cl->onHold = FALSE;
}

#endif

void 
rfbRefuseOnHoldClient(rfbClientPtr cl)
{
    rfbCloseClient(cl);
    rfbClientConnectionGone(cl);
}

static void
rfbDefaultKbdAddEvent(rfbBool down, rfbKeySym keySym, rfbClientPtr cl)
{
}

void
rfbDefaultPtrAddEvent(int buttonMask, int x, int y, rfbClientPtr cl)
{
  rfbClientIteratorPtr iterator;
  rfbClientPtr other_client;

  if (x != cl->screen->cursorX || y != cl->screen->cursorY) {
    if (cl->screen->cursorIsDrawn)
      rfbUndrawCursor(cl->screen);
    LOCK(cl->screen->cursorMutex);
    if (!cl->screen->cursorIsDrawn) {
      cl->screen->cursorX = x;
      cl->screen->cursorY = y;
    }
    UNLOCK(cl->screen->cursorMutex);

    /* The cursor was moved by this client, so don't send CursorPos. */
    if (cl->enableCursorPosUpdates)
      cl->cursorWasMoved = FALSE;

    /* But inform all remaining clients about this cursor movement. */
    iterator = rfbGetClientIterator(cl->screen);
    while ((other_client = rfbClientIteratorNext(iterator)) != NULL) {
      if (other_client != cl && other_client->enableCursorPosUpdates) {
	other_client->cursorWasMoved = TRUE;
      }
    }
    rfbReleaseClientIterator(iterator);
  }
}

void rfbDefaultSetXCutText(char* text, int len, rfbClientPtr cl)
{
}

/* TODO: add a nice VNC or RFB cursor */

#if defined(WIN32) || defined(sparc) || !defined(NO_STRICT_ANSI)
static rfbCursor myCursor = 
{
   FALSE, FALSE, FALSE, FALSE,
   (unsigned char*)"\000\102\044\030\044\102\000",
   (unsigned char*)"\347\347\176\074\176\347\347",
   8, 7, 3, 3,
   0, 0, 0,
   0xffff, 0xffff, 0xffff,
   0
};
#else
static rfbCursor myCursor = 
{
   cleanup: FALSE,
   cleanupSource: FALSE,
   cleanupMask: FALSE,
   cleanupRichSource: FALSE,
   source: "\000\102\044\030\044\102\000",
   mask:   "\347\347\176\074\176\347\347",
   width: 8, height: 7, xhot: 3, yhot: 3,
   foreRed: 0, foreGreen: 0, foreBlue: 0,
   backRed: 0xffff, backGreen: 0xffff, backBlue: 0xffff,
   richSource: 0
};
#endif

rfbCursorPtr rfbDefaultGetCursorPtr(rfbClientPtr cl)
{
   return(cl->screen->cursor);
}

/* response is cl->authChallenge vncEncrypted with passwd */
rfbBool rfbDefaultPasswordCheck(rfbClientPtr cl,const char* response,int len)
{
  int i;
  char *passwd=rfbDecryptPasswdFromFile(cl->screen->authPasswdData);

  if(!passwd) {
    rfbErr("Couldn't read password file: %s\n",cl->screen->authPasswdData);
    return(FALSE);
  }

  rfbEncryptBytes(cl->authChallenge, passwd);

  /* Lose the password from memory */
  for (i = strlen(passwd); i >= 0; i--) {
    passwd[i] = '\0';
  }

  free(passwd);

  if (memcmp(cl->authChallenge, response, len) != 0) {
    rfbErr("authProcessClientMessage: authentication failed from %s\n",
	   cl->host);
    return(FALSE);
  }

  return(TRUE);
}

/* for this method, authPasswdData is really a pointer to an array
   of char*'s, where the last pointer is 0. */
rfbBool rfbCheckPasswordByList(rfbClientPtr cl,const char* response,int len)
{
  char **passwds;
  int i=0;

  for(passwds=(char**)cl->screen->authPasswdData;*passwds;passwds++,i++) {
    uint8_t auth_tmp[CHALLENGESIZE];
    memcpy((char *)auth_tmp, (char *)cl->authChallenge, CHALLENGESIZE);
    rfbEncryptBytes(auth_tmp, *passwds);

    if (memcmp(auth_tmp, response, len) == 0) {
      if(i>=cl->screen->authPasswdFirstViewOnly)
	cl->viewOnly=TRUE;
      return(TRUE);
    }
  }

  rfbErr("authProcessClientMessage: authentication failed from %s\n",
	 cl->host);
  return(FALSE);
}

void rfbDoNothingWithClient(rfbClientPtr cl)
{
}

enum rfbNewClientAction rfbDefaultNewClientHook(rfbClientPtr cl)
{
	return RFB_CLIENT_ACCEPT;
}

rfbBool rfbDefaultProcessCustomClientMessage(rfbClientPtr cl,uint8_t type)
{
	return FALSE;
}

/*
 * Update server's pixel format in screenInfo structure. This
 * function is called from rfbGetScreen() and rfbNewFramebuffer().
 */

static void rfbInitServerFormat(rfbScreenInfoPtr screen, int bitsPerSample)
{
   rfbPixelFormat* format=&screen->serverFormat;

   format->bitsPerPixel = screen->bitsPerPixel;
   format->depth = screen->depth;
   format->bigEndian = rfbEndianTest?FALSE:TRUE;
   format->trueColour = TRUE;
   screen->colourMap.count = 0;
   screen->colourMap.is16 = 0;
   screen->colourMap.data.bytes = NULL;

   if (format->bitsPerPixel == 8) {
     format->redMax = 7;
     format->greenMax = 7;
     format->blueMax = 3;
     format->redShift = 0;
     format->greenShift = 3;
     format->blueShift = 6;
   } else {
     format->redMax = (1 << bitsPerSample) - 1;
     format->greenMax = (1 << bitsPerSample) - 1;
     format->blueMax = (1 << bitsPerSample) - 1;
     if(rfbEndianTest) {
       format->redShift = 0;
       format->greenShift = bitsPerSample;
       format->blueShift = bitsPerSample * 2;
     } else {
       if(format->bitsPerPixel==8*3) {
	 format->redShift = bitsPerSample*2;
	 format->greenShift = bitsPerSample*1;
	 format->blueShift = 0;
       } else {
	 format->redShift = bitsPerSample*3;
	 format->greenShift = bitsPerSample*2;
	 format->blueShift = bitsPerSample;
       }
     }
   }
}

rfbScreenInfoPtr rfbGetScreen(int* argc,char** argv,
 int width,int height,int bitsPerSample,int samplesPerPixel,
 int bytesPerPixel)
{
   rfbScreenInfoPtr screen=malloc(sizeof(rfbScreenInfo));

   INIT_MUTEX(logMutex);

   if(width&3)
     rfbErr("WARNING: Width (%d) is not a multiple of 4. VncViewer has problems with that.\n",width);

   screen->autoPort=FALSE;
   screen->clientHead=0;
   screen->port=5900;
   screen->socketInitDone=FALSE;

   screen->inetdInitDone = FALSE;
   screen->inetdSock=-1;

   screen->udpSock=-1;
   screen->udpSockConnected=FALSE;
   screen->udpPort=0;
   screen->udpClient=0;

   screen->maxFd=0;
   screen->listenSock=-1;

   screen->httpInitDone=FALSE;
   screen->httpEnableProxyConnect=FALSE;
   screen->httpPort=0;
   screen->httpDir=NULL;
   screen->httpListenSock=-1;
   screen->httpSock=-1;

   screen->desktopName = "LibVNCServer";
   screen->alwaysShared = FALSE;
   screen->neverShared = FALSE;
   screen->dontDisconnect = FALSE;
   screen->authPasswdData = 0;
   screen->authPasswdFirstViewOnly = 1;
   
   screen->width = width;
   screen->height = height;
   screen->bitsPerPixel = screen->depth = 8*bytesPerPixel;

   screen->passwordCheck = rfbDefaultPasswordCheck;

   screen->ignoreSIGPIPE = TRUE;

   /* disable progressive updating per default */
   screen->progressiveSliceHeight = 0;

   if(!rfbProcessArguments(screen,argc,argv)) {
     free(screen);
     return 0;
   }

#ifdef WIN32
   {
	   DWORD dummy=255;
	   GetComputerName(screen->thisHost,&dummy);
   }
#else
   gethostname(screen->thisHost, 255);
#endif

   screen->paddedWidthInBytes = width*bytesPerPixel;

   /* format */

   rfbInitServerFormat(screen, bitsPerSample);

   /* cursor */

   screen->cursorIsDrawn = FALSE;
   screen->dontSendFramebufferUpdate = FALSE;
   screen->cursorX=screen->cursorY=screen->underCursorBufferLen=0;
   screen->underCursorBuffer=NULL;
   screen->dontConvertRichCursorToXCursor = FALSE;
   screen->cursor = &myCursor;
   INIT_MUTEX(screen->cursorMutex);

   IF_PTHREADS(screen->backgroundLoop = FALSE);

   screen->deferUpdateTime=5;
   screen->maxRectsPerUpdate=50;

   /* proc's and hook's */

   screen->kbdAddEvent = rfbDefaultKbdAddEvent;
   screen->kbdReleaseAllKeys = rfbDoNothingWithClient;
   screen->ptrAddEvent = rfbDefaultPtrAddEvent;
   screen->setXCutText = rfbDefaultSetXCutText;
   screen->getCursorPtr = rfbDefaultGetCursorPtr;
   screen->setTranslateFunction = rfbSetTranslateFunction;
   screen->newClientHook = rfbDefaultNewClientHook;
   screen->displayHook = 0;
   screen->processCustomClientMessage = rfbDefaultProcessCustomClientMessage;

   /* initialize client list and iterator mutex */
   rfbClientListInit(screen);

   return(screen);
}

/*
 * Switch to another framebuffer (maybe of different size and color
 * format). Clients supporting NewFBSize pseudo-encoding will change
 * their local framebuffer dimensions if necessary.
 * NOTE: Rich cursor data should be converted to new pixel format by
 * the caller.
 */

void rfbNewFramebuffer(rfbScreenInfoPtr screen, char *framebuffer,
                       int width, int height,
                       int bitsPerSample, int samplesPerPixel,
                       int bytesPerPixel)
{
  rfbPixelFormat old_format;
  rfbBool format_changed = FALSE;
  rfbClientIteratorPtr iterator;
  rfbClientPtr cl;

  /* Remove the pointer */

  rfbUndrawCursor(screen);

  /* Update information in the screenInfo structure */

  old_format = screen->serverFormat;

  if (width & 3)
    rfbErr("WARNING: New width (%d) is not a multiple of 4.\n", width);

  screen->width = width;
  screen->height = height;
  screen->bitsPerPixel = screen->depth = 8*bytesPerPixel;
  screen->paddedWidthInBytes = width*bytesPerPixel;

  rfbInitServerFormat(screen, bitsPerSample);

  if (memcmp(&screen->serverFormat, &old_format,
             sizeof(rfbPixelFormat)) != 0) {
    format_changed = TRUE;
  }

  screen->frameBuffer = framebuffer;

  /* Adjust pointer position if necessary */

  if (screen->cursorX >= width)
    screen->cursorX = width - 1;
  if (screen->cursorY >= height)
    screen->cursorY = height - 1;

  /* For each client: */
  iterator = rfbGetClientIterator(screen);
  while ((cl = rfbClientIteratorNext(iterator)) != NULL) {

    /* Re-install color translation tables if necessary */

    if (format_changed)
      screen->setTranslateFunction(cl);

    /* Mark the screen contents as changed, and schedule sending
       NewFBSize message if supported by this client. */

    LOCK(cl->updateMutex);
    sraRgnDestroy(cl->modifiedRegion);
    cl->modifiedRegion = sraRgnCreateRect(0, 0, width, height);
    sraRgnMakeEmpty(cl->copyRegion);
    cl->copyDX = 0;
    cl->copyDY = 0;

    if (cl->useNewFBSize)
      cl->newFBSizePending = TRUE;

    TSIGNAL(cl->updateCond);
    UNLOCK(cl->updateMutex);
  }
  rfbReleaseClientIterator(iterator);
}

#ifdef LIBVNCSERVER_HAVE_LIBJPEG
extern void TightCleanup();
#endif

/* hang up on all clients and free all reserved memory */

void rfbScreenCleanup(rfbScreenInfoPtr screen)
{
  rfbClientIteratorPtr i=rfbGetClientIterator(screen);
  rfbClientPtr cl,cl1=rfbClientIteratorNext(i);
  while(cl1) {
    cl=rfbClientIteratorNext(i);
    rfbClientConnectionGone(cl1);
    cl1=cl;
  }
  rfbReleaseClientIterator(i);
    
#define FREE_IF(x) if(screen->x) free(screen->x)
  FREE_IF(colourMap.data.bytes);
  FREE_IF(underCursorBuffer);
  TINI_MUTEX(screen->cursorMutex);
  if(screen->cursor)
    rfbFreeCursor(screen->cursor);
  free(screen);
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
  rfbTightCleanup();
#endif
}

void rfbInitServer(rfbScreenInfoPtr screen)
{
#ifdef WIN32
  WSADATA trash;
  int i=WSAStartup(MAKEWORD(2,2),&trash);
#endif
  rfbInitSockets(screen);
  rfbHttpInitSockets(screen);
#ifndef __MINGW32__
  if(screen->ignoreSIGPIPE)
    signal(SIGPIPE,SIG_IGN);
#endif
}

#ifndef LIBVNCSERVER_HAVE_GETTIMEOFDAY
#include <fcntl.h>
#include <conio.h>
#include <sys/timeb.h>

void gettimeofday(struct timeval* tv,char* dummy)
{
   SYSTEMTIME t;
   GetSystemTime(&t);
   tv->tv_sec=t.wHour*3600+t.wMinute*60+t.wSecond;
   tv->tv_usec=t.wMilliseconds*1000;
}
#endif

/* defined in rfbserver.c, but kind of "private" */
rfbClientPtr rfbClientIteratorHead(rfbClientIteratorPtr i);

rfbBool
rfbProcessEvents(rfbScreenInfoPtr screen,long usec)
{
  rfbClientIteratorPtr i;
  rfbClientPtr cl,clPrev;
  struct timeval tv;
  rfbBool result=FALSE;

  if(usec<0)
    usec=screen->deferUpdateTime*1000;

  rfbCheckFds(screen,usec);
  rfbHttpCheckFds(screen);
#ifdef CORBA
  corbaCheckFds(screen);
#endif

  i = rfbGetClientIterator(screen);
  cl=rfbClientIteratorHead(i);
  while(cl) {
    if (cl->sock >= 0 && !cl->onHold && FB_UPDATE_PENDING(cl) &&
        !sraRgnEmpty(cl->requestedRegion)) {
      result=TRUE;
      if(screen->deferUpdateTime == 0) {
	  rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
      } else if(cl->startDeferring.tv_usec == 0) {
	gettimeofday(&cl->startDeferring,NULL);
	if(cl->startDeferring.tv_usec == 0)
	  cl->startDeferring.tv_usec++;
      } else {
	gettimeofday(&tv,NULL);
	if(tv.tv_sec < cl->startDeferring.tv_sec /* at midnight */
	   || ((tv.tv_sec-cl->startDeferring.tv_sec)*1000
	       +(tv.tv_usec-cl->startDeferring.tv_usec)/1000)
	     > screen->deferUpdateTime) {
	  cl->startDeferring.tv_usec = 0;
	  rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
	}
      }
    }
    clPrev=cl;
    cl=rfbClientIteratorNext(i);
    if(clPrev->sock==-1) {
      rfbClientConnectionGone(clPrev);
      result=TRUE;
    }
  }
  rfbReleaseClientIterator(i);

  return result;
}

void rfbRunEventLoop(rfbScreenInfoPtr screen, long usec, rfbBool runInBackground)
{
  if(runInBackground) {
#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
       pthread_t listener_thread;

       screen->backgroundLoop = TRUE;

       pthread_create(&listener_thread, NULL, listenerRun, screen);
    return;
#else
    rfbErr("Can't run in background, because I don't have PThreads!\n");
    return;
#endif
  }

  if(usec<0)
    usec=screen->deferUpdateTime*1000;

  while(1)
    rfbProcessEvents(screen,usec);
}
