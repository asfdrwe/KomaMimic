// SPDX-License-Identifier: GPL-3.0-only
#include "book.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv);
 if(argc!=2)return 2;
 QJsonArray runs;
 for(int run=0;run<2;++run) {
  OpenOptions options;options.nestedArchives=true;
  QElapsedTimer timer;timer.start();auto opened=Book::open(QString::fromLocal8Bit(argv[1]),{},options);
  QJsonObject metrics{{"run",run},{"open_ms",timer.nsecsElapsed()/1e6}};
  if(!opened.book){fprintf(stderr,"%s\n",qPrintable(opened.error));return 1;}
  const auto book=opened.book;metrics["pages"]=book->pages.size();QJsonArray timings;
  for(int index:{0,int(book->pages.size()/2),int(book->pages.size()-1),0,int(book->pages.size()/2),int(book->pages.size()-1)}) {
   timer.restart();const auto image=book->image(index);
   if(!image.error.isEmpty()){fprintf(stderr,"%s\n",qPrintable(image.error));return 1;}
   timings.append(QJsonObject{{"page",index},{"ms",timer.nsecsElapsed()/1e6}});
  }
  metrics["decode"]=timings;runs.append(metrics);
 }
 QJsonObject report{{"runs",runs}};
#ifdef Q_OS_LINUX
 rusage usage{};getrusage(RUSAGE_SELF,&usage);report["process_peak_rss_kib"]=double(usage.ru_maxrss);
#endif
 const auto output=QJsonDocument(report).toJson();fwrite(output.constData(),1,output.size(),stdout);return 0;
}
