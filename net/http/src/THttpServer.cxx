// $Id$
// Author: Sergey Linev   21/12/2013

#include "THttpServer.h"

#include "TTimer.h"
#include "TSystem.h"
#include "TImage.h"
#include "TROOT.h"
#include "TClass.h"

#include "THttpEngine.h"
#include "TRootSniffer.h"
#include "TRootSnifferStore.h"

#include <string>
#include <cstdlib>

//extern "C" void R__zipMultipleAlgorithm(int cxlevel, int *srcsize, char *src, int *tgtsize, char *tgt, int *irep, int compressionAlgorithm);
//const Int_t kMAXBUF = 0xffffff;


//////////////////////////////////////////////////////////////////////////
//                                                                      //
// THttpCallArg                                                         //
//                                                                      //
// Contains arguments for single HTTP call                              //
// Must be used in THttpEngine to process icomming http requests        //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//______________________________________________________________________________
THttpCallArg::THttpCallArg() :
   TObject(),
   fTopName(),
   fPathName(),
   fFileName(),
   fQuery(),
   fCond(),
   fContentType(),
   fContentEncoding(),
   fContent(),
   fBinData(0),
   fBinDataLength(0)
{
   // constructor
}

//______________________________________________________________________________
THttpCallArg::~THttpCallArg()
{
   // destructor

   if (fBinData) {
      free(fBinData);
      fBinData = 0;
   }
}

//______________________________________________________________________________
void THttpCallArg::SetPathAndFileName(const char *fullpath)
{
   // set complete path of requested http element
   // For instance, it could be "/folder/subfolder/get.bin"
   // Here "/folder/subfolder/" is element path and "get.bin" requested file.
   // One could set path and file name separately

   fPathName.Clear();
   fFileName.Clear();

   if (fullpath == 0) return;

   const char *rslash = strrchr(fullpath, '/');
   if (rslash == 0) {
      fFileName = fullpath;
   } else {
      while ((fullpath != rslash) && (*fullpath == '/')) fullpath++;
      fPathName.Append(fullpath, rslash - fullpath);
      if (fPathName == "/") fPathName.Clear();
      fFileName = rslash + 1;
   }
}

//______________________________________________________________________________
void THttpCallArg::FillHttpHeader(TString &hdr, Bool_t normal)
{
   const char *header = normal ? "HTTP/1.1" : "Status:";

   if ((fContentType.Length() == 0) || Is404()) {
      hdr.Form("%s 404 Not Found\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n\r\n", header);
      return;
   }

   hdr.Form("%s 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Connection: keep-alive\r\n"
            "Content-Length: %ld\r\n",
            header,
            GetContentType(),
            GetContentLength());
   if (fContentEncoding.Length() > 0)
      hdr.Append(TString::Format("Content-Encoding: %s\r\n", fContentEncoding.Data()));

   hdr.Append("\r\n");
}


// ====================================================================

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// THttpTimer                                                           //
//                                                                      //
// Specialized timer for THttpServer                                    //
// Main aim - provide regular call of THttpServer::ProcessRequests()    //
// method                                                               //
//                                                                      //
//////////////////////////////////////////////////////////////////////////


//______________________________________________________________________________
class THttpTimer : public TTimer {
public:

   THttpServer *fServer;

   THttpTimer(Long_t milliSec, Bool_t mode, THttpServer *serv) :
      TTimer(milliSec, mode), fServer(serv)
   {
      // construtor
   }
   virtual ~THttpTimer()
   {
      // destructor
   }
   virtual void Timeout()
   {
      // timeout handler
      // used to process http requests in main ROOT thread

      if (fServer) fServer->ProcessRequests();
   }
};

// =======================================================

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// THttpServer                                                          //
//                                                                      //
// Online server for arbitrary ROOT analysis                            //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//______________________________________________________________________________
THttpServer::THttpServer(const char *engine) :
   TNamed("http", "ROOT http server"),
   fEngines(),
   fTimer(0),
   fSniffer(0),
   fMainThrdId(0),
   fHttpSys(),
   fRootSys(),
   fJSRootIOSys(),
   fTopName("ROOT"),
   fMutex(),
   fCallArgs()
{
   // constructor

   // Checks where ROOT sources (via $ROOTSYS variable)
   // Sources are required to locate files and scripts,
   // which will be provided to the web clients by request

   fMainThrdId = TThread::SelfId();

   // Info("THttpServer", "Create %p in thrd %ld", this, (long) fMainThrdId);

   const char *rootsys = gSystem->Getenv("ROOTSYS");
   if (rootsys != 0) fRootSys = rootsys;

#ifdef COMPILED_WITH_DABC

   const char *dabcsys = gSystem->Getenv("DABCSYS");
   if (dabcsys != 0) fHttpSys = TString::Format("%s/plugins/http", dabcsys);

#else
   if (fRootSys.Length() > 0)
      fHttpSys = TString::Format("%s/etc/http", fRootSys.Data());
#endif

   if (fHttpSys.IsNull()) fHttpSys = ".";

   const char *jsrootiosys = gSystem->Getenv("JSROOTIOSYS");
   if (jsrootiosys != 0)
      fJSRootIOSys = jsrootiosys;
   else
      fJSRootIOSys = fHttpSys + "/JSRootIO";

   fDefaultPage = fHttpSys + "/files/main.htm";
   fDrawPage = fHttpSys + "/files/single.htm";

   SetSniffer(new TRootSniffer("sniff"));

   // start timer
   SetTimer(100, kTRUE);

   CreateEngine(engine);
}

//______________________________________________________________________________
THttpServer::~THttpServer()
{
   // destructor
   // delete all http engines and sniffer

   fEngines.Delete();

   SetSniffer(0);

   SetTimer(0);
}

//______________________________________________________________________________
void THttpServer::SetSniffer(TRootSniffer *sniff)
{
   if (fSniffer) delete fSniffer;
   fSniffer = sniff;
}

//______________________________________________________________________________
Bool_t THttpServer::CreateEngine(const char *engine)
{
   // factory method to create different http engine
   // At the moment two engine kinds are supported:
   //  civetweb (default) and fastcgi
   // Examples:
   //   "civetweb:8090" or "http:8090" or ":8090" - creates civetweb web server with http port 8090
   //   "fastcgi:9000" - creates fastcgi server with port 9000
   //   "dabc:1237"   - create DABC server with port 1237

   if (engine == 0) return kFALSE;

   const char *arg = strchr(engine, ':');
   if (arg == 0) return kFALSE;

   TString clname;
   if (arg != engine) clname.Append(engine, arg - engine);

   if ((clname.Length() == 0) || (clname == "http") || (clname == "civetweb"))
      clname = "TCivetweb";
   else if (clname == "fastcgi")
      clname = "TFastCgi";
   else if (clname == "dabc")
      clname = "TDabcEngine";

   // ensure that required engine class exists before we try to create it
   TClass *engine_class = gROOT->LoadClass(clname.Data());
   if (engine_class == 0) return kFALSE;

   THttpEngine *eng = (THttpEngine *) engine_class->New();
   if (eng == 0) return kFALSE;

   eng->SetServer(this);

   if (!eng->Create(arg + 1)) {
      delete eng;
      return kFALSE;
   }

   fEngines.Add(eng);

   return kTRUE;
}

//______________________________________________________________________________
void THttpServer::SetTimer(Long_t milliSec, Bool_t mode)
{
   // create timer which will invoke ProcessRequests() function periodically
   // Timer is required to perform all actions in main ROOT thread
   // Method arguments are the same as for TTimer constructor
   // By default, sync timer with 100 ms period is created
   //
   // If milliSec == 0, no timer will be created.
   // In this case application should regularly call ProcessRequests() method.

   if (fTimer) {
      fTimer->Stop();
      delete fTimer;
      fTimer = 0;
   }
   if (milliSec > 0) {
      fTimer = new THttpTimer(milliSec, mode, this);
      fTimer->TurnOn();
   }
}

//______________________________________________________________________________
Bool_t THttpServer::IsFileRequested(const char *uri, TString &res) const
{
   // verifies that request just file name
   // File names typically contains prefix like "httpsys/" or "jsrootiosys/"
   // If true, method returns real name of the file,
   // which should be delivered to the client
   // Method is thread safe and can be called from any thread

   if ((uri == 0) || (strlen(uri) == 0)) return kFALSE;

   std::string fname = uri;
   size_t pos = fname.rfind("httpsys/");
   if (pos != std::string::npos) {
      fname.erase(0, pos + 7);
      res = fHttpSys + fname.c_str();
      return kTRUE;
   }

   if (!fRootSys.IsNull()) {
      pos = fname.rfind("rootsys/");
      if (pos != std::string::npos) {
         fname.erase(0, pos + 7);
         res = fRootSys + fname.c_str();
         return kTRUE;
      }
   }

   if (!fJSRootIOSys.IsNull()) {
      pos = fname.rfind("jsrootiosys/");
      if (pos != std::string::npos) {
         fname.erase(0, pos + 11);
         res = fJSRootIOSys + fname.c_str();
         return kTRUE;
      }
   }

   return kFALSE;
}

//______________________________________________________________________________
Bool_t THttpServer::ExecuteHttp(THttpCallArg *arg)
{
   // Executes http request, specified in THttpCallArg structure
   // Method can be called from any thread
   // Actual execution will be done in main ROOT thread, where analysis code is running.

   if (fMainThrdId == TThread::SelfId()) {
      // should not happen, but one could process requests directly without any signaling

      ProcessRequest(arg);

      return kTRUE;
   }

   // add call arg to the list
   fMutex.Lock();
   fCallArgs.Add(arg);
   fMutex.UnLock();

   // and now wait until request is processed
   arg->fCond.Wait();

   return kTRUE;
}

//______________________________________________________________________________
void THttpServer::ProcessRequests()
{
   // Process requests, submitted for execution
   // Regularly invoked by THttpTimer, when somewhere in the code
   // gSystem->ProcessEvents() is called.
   // User can call serv->ProcessRequests() directly, but only from main analysis thread.

   // Info("ProcessRequests", "Server %p in main %ld curr %ld", this, (long) fMainThrdId, (long)TThread::SelfId());

   if (fMainThrdId != TThread::SelfId()) {
      Error("ProcessRequests", "Should be called only from main ROOT thread");
      return;
   }

   while (true) {
      THttpCallArg *arg = 0;

      fMutex.Lock();
      if (fCallArgs.GetSize() > 0) {
         arg = (THttpCallArg *) fCallArgs.First();
         fCallArgs.RemoveFirst();
      }
      fMutex.UnLock();

      if (arg == 0) break;

      ProcessRequest(arg);

      arg->fCond.Signal();
   }

   // regularly call Process() method of engine to let perform actions in ROOT context
   TIter iter(&fEngines);
   THttpEngine *engine = 0;
   while ((engine = (THttpEngine *)iter()) != 0)
      engine->Process();
}

//______________________________________________________________________________
void THttpServer::ProcessRequest(THttpCallArg *arg)
{
   // Process single http request
   // Depending from requested path and filename different actions will be performed.
   // In most cases information is provided by TRootSniffer class

   // Info("ProcessRequest", "Path %s File %s", arg->fPathName.Data(), arg->fFileName.Data());

   if (arg->fFileName.IsNull() || (arg->fFileName == "index.htm")) {

      Bool_t usedefaultpage = kTRUE;

      if (!fSniffer->CanExploreItem(arg->fPathName.Data()) &&
            fSniffer->CanDrawItem(arg->fPathName.Data())) usedefaultpage = kFALSE;

      if (usedefaultpage)
         arg->fContent = fDefaultPage;
      else
         arg->fContent = fDrawPage;

      arg->SetFile();

      return;
   }

   TString buf = arg->fFileName;

   if (IsFileRequested(arg->fFileName.Data(), buf)) {
      arg->fContent = buf;
      arg->SetFile();
      return;
   }

   if (arg->fFileName == "h.xml")  {

      arg->fContent.Form(
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<dabc version=\"2\" xmlns:dabc=\"http://dabc.gsi.de/xhtml\" path=\"%s\">\n", arg->fPathName.Data());

      {
         TRootSnifferStoreXml store(arg->fContent);

         const char *topname = fTopName.Data();
         if (arg->fTopName.Length() > 0) topname = arg->fTopName.Data();

         fSniffer->ScanHierarchy(topname, arg->fPathName.Data(), &store);
      }

      arg->fContent.Append("</dabc>\n");
      arg->SetXml();

      return;
   }

   if (arg->fFileName == "h.json")  {

      arg->fContent.Append("{\n");

      {
         TRootSnifferStoreJson store(arg->fContent);

         const char *topname = fTopName.Data();
         if (arg->fTopName.Length() > 0) topname = arg->fTopName.Data();

         fSniffer->ScanHierarchy(topname, arg->fPathName.Data(), &store);
      }

      arg->fContent.Append("\n}\n");
      arg->SetJson();

      return;
   }

   if (fSniffer->Produce(arg->fPathName.Data(), arg->fFileName.Data(), arg->fQuery.Data(), arg->fBinData, arg->fBinDataLength)) {
      // define content type base on extension
      arg->SetContentType(GetMimeType(arg->fFileName.Data()));
      return;
   }

   /*
     if (arg->fFileName == "get.json.gz") {
        if (fSniffer->ProduceJson(arg->fPathName.Data(), arg->fQuery.Data(), arg->fContent)) {

           Int_t fObjlen = arg->fContent.Length();
           Int_t cxlevel = 5;
           Int_t cxAlgorithm = 0;

           Int_t nbuffers = 1 + (fObjlen - 1)/kMAXBUF;
           Int_t buflen = fObjlen + 9*nbuffers + 28; //add 28 bytes in case object is placed in a deleted gap
           if (buflen<512) buflen = 512;

           void* fBuffer = malloc(buflen);

           char *objbuf = (char*) arg->fContent.Data();
           char *bufcur = (char*) fBuffer;
           Int_t noutot = 0;
           Int_t nzip   = 0;
           for (Int_t i = 0; i < nbuffers; ++i) {
              Int_t bufmax = kMAXBUF, nout(0);
              if (i == nbuffers - 1) bufmax = fObjlen - nzip;
              R__zipMultipleAlgorithm(cxlevel, &bufmax, objbuf, &bufmax, bufcur, &nout, cxAlgorithm);
              if (nout == 0 || nout >= fObjlen) { //this happens when the buffer cannot be compressed
                 Error("", "Fail to compress data");
                 free(fBuffer);
                 return;
              }
              bufcur += nout;
              noutot += nout;
              objbuf += kMAXBUF;
              nzip   += kMAXBUF;
           }

           Info("Compress", "Original size %d compressed %d", fObjlen, noutot);

           arg->fBinData = fBuffer;
           arg->fBinDataLength = noutot;
           arg->fContent.Clear();

           arg->SetEncoding("gzip");
           arg->SetJson();
           return;
        }
     }
   */

   arg->Set404();
}

//______________________________________________________________________________
Bool_t THttpServer::Register(const char *subfolder, TObject *obj)
{
   // Register object in folders hierarchy
   //
   // See TRootSniffer::RegisterObject() for more details

   return fSniffer->RegisterObject(subfolder, obj);
}

//______________________________________________________________________________
Bool_t THttpServer::Unregister(TObject *obj)
{
   // Unregister object in folders hierarchy
   //
   // See TRootSniffer::UnregisterObject() for more details

   return fSniffer->UnregisterObject(obj);
}


//______________________________________________________________________________
const char *THttpServer::GetMimeType(const char *path)
{
   static const struct {
      const char *extension;
      int ext_len;
      const char *mime_type;
   } builtin_mime_types[] = {
      {".xml", 4, "text/xml"},
      {".json", 5, "application/json"},
      {".bin", 4, "application/x-binary"},
      {".gif", 4, "image/gif"},
      {".jpg", 4, "image/jpeg"},
      {".png", 4, "image/png"},
      {".html", 5, "text/html"},
      {".htm", 4, "text/html"},
      {".shtm", 5, "text/html"},
      {".shtml", 6, "text/html"},
      {".css", 4, "text/css"},
      {".js",  3, "application/x-javascript"},
      {".ico", 4, "image/x-icon"},
      {".jpeg", 5, "image/jpeg"},
      {".svg", 4, "image/svg+xml"},
      {".txt", 4, "text/plain"},
      {".torrent", 8, "application/x-bittorrent"},
      {".wav", 4, "audio/x-wav"},
      {".mp3", 4, "audio/x-mp3"},
      {".mid", 4, "audio/mid"},
      {".m3u", 4, "audio/x-mpegurl"},
      {".ogg", 4, "application/ogg"},
      {".ram", 4, "audio/x-pn-realaudio"},
      {".xslt", 5, "application/xml"},
      {".xsl", 4, "application/xml"},
      {".ra",  3, "audio/x-pn-realaudio"},
      {".doc", 4, "application/msword"},
      {".exe", 4, "application/octet-stream"},
      {".zip", 4, "application/x-zip-compressed"},
      {".xls", 4, "application/excel"},
      {".tgz", 4, "application/x-tar-gz"},
      {".tar", 4, "application/x-tar"},
      {".gz",  3, "application/x-gunzip"},
      {".arj", 4, "application/x-arj-compressed"},
      {".rar", 4, "application/x-arj-compressed"},
      {".rtf", 4, "application/rtf"},
      {".pdf", 4, "application/pdf"},
      {".swf", 4, "application/x-shockwave-flash"},
      {".mpg", 4, "video/mpeg"},
      {".webm", 5, "video/webm"},
      {".mpeg", 5, "video/mpeg"},
      {".mov", 4, "video/quicktime"},
      {".mp4", 4, "video/mp4"},
      {".m4v", 4, "video/x-m4v"},
      {".asf", 4, "video/x-ms-asf"},
      {".avi", 4, "video/x-msvideo"},
      {".bmp", 4, "image/bmp"},
      {".ttf", 4, "application/x-font-ttf"},
      {NULL,  0, NULL}
   };

   int path_len = strlen(path);

   for (int i = 0; builtin_mime_types[i].extension != NULL; i++) {
      if (path_len <= builtin_mime_types[i].ext_len) continue;
      const char *ext = path + (path_len - builtin_mime_types[i].ext_len);
      if (strcmp(ext, builtin_mime_types[i].extension) == 0) {
         return builtin_mime_types[i].mime_type;
      }
   }

   return "text/plain";
}
