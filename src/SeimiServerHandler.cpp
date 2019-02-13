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
#include <QJsonArray>
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
    outImgSizeP("outImgSize"),
    uaP("ua"),
    resourceTimeoutP("resourceTimeout")
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

    //获取请求内容，便于使用json解析
    QString reqContent = connection->requestContent();
    QJsonParseError json_error;
    QJsonDocument jDoc = QJsonDocument::fromJson(reqContent.toStdString().c_str(), &json_error);
    if(json_error.error != QJsonParseError::NoError or !jDoc.isObject()){
        qInfo("[Seimi] parameters must be json format［Params = %s", reqContent.toUtf8().constData());
        connection->writeResponse(501, Pillow::HttpHeaderCollection(),"parameters must be json format!");
        return true;
    }
    QJsonObject params = jDoc.object();

    qInfo("[Seimi] Params:%s", reqContent.toUtf8().constData());

    QString url = QUrl::fromPercentEncoding(params.take("url").toString().toUtf8());
    int renderTime = params.take("wait").toInt();
    QString proxyStr = params.take("proxy").toString();
    QString contentType = params.take("contentType").toString();
    QString outImgSizeStr = params.take("outImgSize").toString();
    QString ua = connection->requestParamValue(uaP);
    if(ua.isEmpty()){
        ua = "Mozilla/5.0 (Windows NT 6.3; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/53.0.2785.116 Safari/537.36";
    }
    qInfo("[Seimi] UserAgent:%s", ua.toUtf8().constData());
//    QString jscript = QUrl::fromPercentEncoding(connection->requestParamValue(scriptP).toUtf8());
    QString jscript = "(" + QUrl::fromPercentEncoding(params.take("js_script").toString().toUtf8()) +")()";
    if(jscript == "()()"){
        jscript = "";
    }
    QString postParamJson = "{\"" + params.take("data").toString().replace("=", "\":\"").replace("&", "\",\"") +"\"}";
    if(postParamJson == "{\"\"}"){
        postParamJson = "";
    }else{
        qInfo("[Seimi] postParamJson:%s", postParamJson.toUtf8().constData());
    }
    int resourceTimeout = params.take("timeout").toInt();
    Pillow::HttpHeaderCollection headers;
    headers << Pillow::HttpHeader("Pragma", "no-cache");
    headers << Pillow::HttpHeader("Expires", "-1");
    headers << Pillow::HttpHeader("Cache-Control", "no-cache");
    try{
        QEventLoop eventLoop;
        SeimiPage *seimiPage=new SeimiPage(this);
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
                qWarning("[Seimi] proxy pattern error, proxy = %s",proxyStr.toUtf8().constData());
            }
        }
        seimiPage->setScript(jscript);
        seimiPage->setPostParam(postParamJson);
        qInfo("[Seimi] TargetUrl:%s ,RenderTime(ms):%d",url.toUtf8().constData(),renderTime);
        int useCookieFlag = connection->requestParamValue(useCookieP).toInt();
        seimiPage->setUseCookie(useCookieFlag==1);
        QObject::connect(seimiPage,SIGNAL(loadOver()),&eventLoop,SLOT(quit()));
        seimiPage->toLoad(url,renderTime,ua,resourceTimeout);
        eventLoop.exec();

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
            headers << Pillow::HttpHeader("Content-Type", "application/json;charset=utf-8");



            QString content = seimiPage->getContent();
            qInfo()<<"[Seimi] content:"<<content.replace("\n", "\\n").left(200) + " ...";

            int timeDiff = qTime.elapsed();
            float f = timeDiff/1000.0;
            QString time = QString("%2").arg(f);
            QString jsresult = seimiPage->getJSResult();
            QJsonDocument jDocJSResult;
            int jsflag = 0;
            if((jsresult.startsWith("{") and jsresult.endsWith("}")) or (jsresult.startsWith("[") and jsresult.endsWith("]"))){
                QJsonParseError json_error2;
                jDocJSResult = QJsonDocument::fromJson(jsresult.toStdString().c_str(), &json_error);
                if(!(json_error2.error != QJsonParseError::NoError or (!jDocJSResult.isObject() and !jDocJSResult.isArray()))){
                    jsflag = 1;
                }
            }
            QJsonObject data;
            data.insert("content", content);
            if(jsflag == 1){
                if(jDocJSResult.isObject()){
                    data.insert("js_script_result", QJsonValue(jDocJSResult.object()));
                } else {
                    data.insert("js_script_result", QJsonValue(jDocJSResult.array()));
                }
            } else {
                data.insert("js_script_result", jsresult);
            }
            data.insert("orig_url", url);
            data.insert("url", seimiPage->getCurrentUrl());
            data.insert("status_code", (content.isEmpty() ? "999" : "200"));
            data.insert("cookies", "");
            data.insert("cookiesString", "");
            data.insert("time", time);

            QJsonDocument document;
            document.setObject(data);
            QByteArray byteArray = document.toJson(QJsonDocument::Compact);
            QString strJson(byteArray);

            /*
            QString data1 = "{\n\t\"content\":\"" + content.replace("\"", "\\\"") + "\",\n"
                           + "\t\"js_script_result\":" + jsresult + ",\n"
                           + "\t\"orig_url\":\"" + url + "\"" + ",\n"
                           + "\t\"url\":\"" + url + "\"" + ",\n"
                           + "\t\"status_code\":"+ (content.isEmpty() ? "999" : "200") + ",\n"
                           + "\t\"cookies\":\"" + "" + "\"" + ",\n"
                           + "\t\"cookiesString\":\"" + "" + "\"" + ",\n"
                           + "\t\"time\":" + time
                           + "\n}";
                           */

            connection->writeResponse(200, headers, strJson.replace("\\\\n", "\\n").toUtf8());
            qInfo("[Seimi] finished, cost:%ss", time.toUtf8().constData());
        }
        seimiPage->deleteLater();
    }catch (std::exception& e) {
        headers << Pillow::HttpHeader("Content-Type", "text/html;charset=utf-8");
        QString errMsg = "<html>server error,please try again.</html>";
        qInfo("[Seimi error] Page error, url: %s, errorMsg: %s", url.toUtf8().constData(), QString(QLatin1String(e.what())).toUtf8().constData());
        connection->writeResponse(500, headers, errMsg.toUtf8());
    }catch (...) {
        qInfo() << "server error!";
        headers << Pillow::HttpHeader("Content-Type", "text/html;charset=utf-8");
        QString errMsg = "<html>server error,please try again.</html>";
        qInfo("[Seimi error] Page error, url: %s, errorMsg: unknow", url.toUtf8().constData());
        connection->writeResponse(500, headers, errMsg.toUtf8());
    }

    return true;
}

