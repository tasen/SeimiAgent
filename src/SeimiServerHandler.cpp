/*
   Copyright 2016 Wang Haomiao<et.tw@163.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */
#include <QObject>
#include <QEventLoop>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTime>
#include "SeimiServerHandler.h"
#include "SeimiWebPage.h"
#include "pillowcore/HttpServer.h"
#include "pillowcore/HttpHandler.h"
#include "pillowcore/HttpConnection.h"


SeimiServerHandler::SeimiServerHandler(QObject *parent):Pillow::HttpHandler(parent),
    renderTimeP("renderTime"),
    urlP("url"),
    proxyP("proxy"),
    scriptP("script"),
    useCookieP("useCookie"),
    postParamP("postParam"),
    contentTypeP("contentType"),
    outImgSizeP("outImgSize")
{

}

bool SeimiServerHandler::handleRequest(Pillow::HttpConnection *connection){
    QTime qTime;
    qTime.start();

    QString method = connection->requestMethod();
    QString path = connection->requestPath();
    if(method == "GET"){
        connection->writeResponse(405, Pillow::HttpHeaderCollection(),"Method 'GET' is not supprot,please use 'POST'");
        return true;
    }
    if(path != "/" and path != ""){
        connection->writeResponse(404, Pillow::HttpHeaderCollection(),"page not found!");
        return true;
    }
    QEventLoop eventLoop;
    SeimiPage *seimiPage=new SeimiPage(this);

    //获取请求内容，便于使用json解析
    QString reqContent = connection->requestContent();
    QJsonParseError json_error;
    QJsonDocument jDoc = QJsonDocument::fromJson(reqContent.toStdString().c_str(), &json_error);
    if(json_error.error != QJsonParseError::NoError or !jDoc.isObject()){
        qInfo()<<"[Seimi] parameters must be json format［Params = "+reqContent+"］";
        connection->writeResponse(501, Pillow::HttpHeaderCollection(),"parameters must be json format!");
        return true;
    }
    QJsonObject params = jDoc.object();

    qInfo()<<"[Seimi] Params:"+reqContent;

    QString url = QUrl::fromPercentEncoding(params.take("url").toString().toUtf8());
    int renderTime = params.take("wait").toInt()*1000;
    QString proxyStr = params.take("proxy").toString();
    QString contentType = params.take("contentType").toString();
    QString outImgSizeStr = params.take("outImgSize").toString();
    if(!proxyStr.isEmpty()){
        if(proxyStr.indexOf("://") == -1){
            proxyStr = "http://" + proxyStr;
        }
        QRegularExpression reProxy("(?<protocol>http|https|socket)://(?:(?<user>\\w*):(?<password>\\w*)@)?(?<host>[\\w.]+)(:(?<port>\\d+))?");
        QRegularExpressionMatch matchProxy = reProxy.match(proxyStr);
        if(matchProxy.hasMatch()){
            QNetworkProxy proxy;
            if(matchProxy.captured("protocol") == "socket"){
                proxy.setType(QNetworkProxy::Socks5Proxy);
            }else{
                proxy.setType(QNetworkProxy::HttpProxy);
            }
            proxy.setHostName(matchProxy.captured("host"));
            proxy.setPort(matchProxy.captured("port").toInt()==0?80:matchProxy.captured("port").toInt());
            proxy.setUser(matchProxy.captured("user"));
            proxy.setPassword(matchProxy.captured("password"));

            seimiPage->setProxy(proxy);
        }else {
            qWarning("[seimi] proxy pattern error, proxy = %s",proxyStr.toUtf8().constData());
        }
    }

    //QString jscript = QUrl::fromPercentEncoding(connection->requestParamValue(scriptP).toUtf8());
    QString jscript = "(" + QUrl::fromPercentEncoding(params.take("js_script").toString().toUtf8()) +")()";
    if(jscript == "()()"){
        jscript = "";
    }
//    qDebug()<<"recive js:"<<jscript;
    QString postParamJson = "{\"" + params.take("data").toString().replace("=", "\":\"").replace("&", "\",\"") +"\"}";
    if(postParamJson == "{\"\"}"){
        postParamJson = "";
    }else{
        qInfo()<<"[Seimi] postParamJson:" + postParamJson;
    }
    seimiPage->setScript(jscript);
    seimiPage->setPostParam(postParamJson);
    qInfo("[Seimi] TargetUrl:%s ,RenderTime(ms):%d",url.toUtf8().constData(),renderTime);
    int useCookieFlag = connection->requestParamValue(useCookieP).toInt();
    seimiPage->setUseCookie(useCookieFlag==1);
    QObject::connect(seimiPage,SIGNAL(loadOver()),&eventLoop,SLOT(quit()));
    seimiPage->toLoad(url,renderTime);
    eventLoop.exec();
    Pillow::HttpHeaderCollection headers;
    headers << Pillow::HttpHeader("Pragma", "no-cache");
    headers << Pillow::HttpHeader("Expires", "-1");
    headers << Pillow::HttpHeader("Cache-Control", "no-cache");
    if(contentType == "pdf"){
        headers << Pillow::HttpHeader("Content-Type", "application/pdf");
        QByteArray pdfContent = seimiPage->generatePdf();
        QCryptographicHash md5sum(QCryptographicHash::Md5);
        md5sum.addData(pdfContent);
        QByteArray etag = md5sum.result().toHex();
        headers << Pillow::HttpHeader("ETag", etag);
        connection->writeResponse(200,headers,pdfContent);
    }else if(contentType == "img"){
        headers << Pillow::HttpHeader("Content-Type", "image/png");
        QSize targetSize;
        if(!outImgSizeStr.isEmpty()){
            QRegularExpression reImgSize("(?<xSize>\\d+)(?:x|X)(?<ySize>\\d+)");
            QRegularExpressionMatch matchImgSize = reImgSize.match(outImgSizeStr);
            if(matchImgSize.hasMatch()){
                targetSize.setWidth(matchImgSize.captured("xSize").toInt());
                targetSize.setHeight(matchImgSize.captured("ySize").toInt());
            }
        }
        QByteArray imgContent = seimiPage->generateImg(targetSize);
        QCryptographicHash md5sum(QCryptographicHash::Md5);
        md5sum.addData(imgContent);
        QByteArray etag = md5sum.result().toHex();
        headers << Pillow::HttpHeader("ETag", etag);
        connection->writeResponse(200,headers,imgContent);
    }else{
        headers << Pillow::HttpHeader("Content-Type", "text/html;charset=utf-8");
        QString defBody = "{\n\t\"content\":\"null\"\n}";
        QString content = seimiPage->getContent();
        qInfo()<<"[Seimi] content:"<<content.replace("\n", "\\n").left(200) + " ...";

        int timeDiff = qTime.elapsed();
        float f = timeDiff/1000.0;
        QString time = QString("%2").arg(f);

        QString data = "{\n\t\"content\":\"" + content.replace("\"", "\\\"") + "\",\n"
                       + "\t\"js_script_result\":\"" + seimiPage->getJSResult() + "\"" + ",\n"
                       + "\t\"orig_url\":\"" + url + "\"" + ",\n"
                       + "\t\"url\":\"" + url + "\"" + ",\n"
                       + "\t\"status_code\":\"200\"" + ",\n"
                       + "\t\"cookies\":\"" + "" + "\"" + ",\n"
                       + "\t\"cookiesString\":\"" + "" + "\"" + ",\n"
                       + "\t\"time\":" + time
                       + "\n}";
        connection->writeResponse(200, headers,seimiPage->getContent().isEmpty()?defBody.toUtf8():data.toUtf8());
    }
    seimiPage->deleteLater();
    return true;
}

