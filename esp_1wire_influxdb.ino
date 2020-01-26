/*
 * esp8266 1wire to InfluxDB client
 * 
 * See https://github.com/oh2mp/esp_1wire_influxdb
*/

#include <Arduino.h>
#include <OneWire.h> 
#include <DallasTemperature.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <DNSServer.h>   
#include <ESP8266WebServer.h>
#include <FS.h>
#include <base64.h>

#include "strutils.h"

/* ------------------------------------------------------------------------------- */
/* These are the pins for all ESP8266 boards */
//      Name   GPIO    Function
#define PIN_D0  16  // WAKE
#define PIN_D1   5  // User purpose
#define PIN_D2   4  // User purpose
#define PIN_D3   0  // Low on boot means enter FLASH mode
#define PIN_D4   2  // TXD1 (must be high on boot to go to UART0 FLASH mode)
#define PIN_D5  14  // HSCLK
#define PIN_D6  12  // HMISO
#define PIN_D7  13  // HMOSI  RXD2
#define PIN_D8  15  // HCS    TXD0 (must be low on boot to enter UART0 FLASH mode)
#define PIN_D9   3  //        RXD0
#define PIN_D10  1  //        TXD0

#define PIN_MOSI 8  // SD1
#define PIN_MISO 7  // SD0
#define PIN_SCLK 6  // CLK
#define PIN_HWCS 0  // D3

#define PIN_D11  9  // SD2
#define PIN_D12 10  // SD4
/* ------------------------------------------------------------------------------- */

#define ONE_WIRE_BUS PIN_D2
#define APREQUEST PIN_D1
#define APTIMEOUT 180000
#define MAX_SENSORS 8

float sens[MAX_SENSORS];        // sensor values
char sensname[MAX_SENSORS][24]; // sensor names
int sread = 0;                  // Flag if sensors have been read on this iteration
char temp[8] = "\0";            // Used for temperature value to print
int onewire_wait = 1;           // if we are waiting for 1wire data
unsigned long mytime = 0;       // Used for delaying, see loop function
int scount = 0;                 // sensors amount
char postmsg[256] = "";         // buffer for message to send

char currssid[33];              // current SSID
char currip[16];                // current IP address
char lastssid[33];              // last SSID
char lastip[16];                // last IP address

char measurement[32];           // measurement name for InfluxDB
char userpass[64];              // user:pass for InfluxDB
String b64pass;                 // base64 encoded user:pass for basic auth
char intervalstr[6];            // interval as string
int interval = 0;               // interval as minutes

url_info urlp;
char url[128];
char scheme[6];
char host[64];
char path[64];
uint16_t port;

unsigned long portal_timer = 0;
unsigned long lastpacket = 0;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress sensor[8];

ESP8266WiFiMulti WiFiMulti;
BearSSL::WiFiClientSecure httpsclient;
WiFiClient httpclient;

ESP8266WebServer server(80);
IPAddress apIP(192,168,4,1);   // portal ip address
DNSServer dnsServer;
File file;

const int API_TIMEOUT = 10000; // timeout in milliseconds for http/https client

/* ------------------------------------------------------------------------------- */
/* get sensor index from hexstring like "10F78DBA000800D7" */
int getSensorIndex(const char *hexString) {
    char tmp[3];
    char saddrstr[17];
    memset(saddrstr,0,sizeof(saddrstr));
    for (int i = 0; i < scount; i++) {
        for (uint8_t j = 0; j < 8; j++) {
            sprintf(tmp,"%02X",sensor[i][j]);
            tmp[2] = 0;
            strcat(saddrstr,tmp);
        }
        if (strcmp(saddrstr,hexString) == 0) {
            return i;
        } else {
            memset(saddrstr,0,sizeof(saddrstr));
        }
    }
    return -1; // no sensor found
}
/* ------------------------------------------------------------------------------- */
void loadSavedSensors() {
   char sname[25];
   char saddrstr[17];
    
   if (SPIFFS.exists("/known_sensors.txt")) {
        file = SPIFFS.open("/known_sensors.txt", "r");
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(saddrstr, '\0', sizeof(saddrstr));
            
            file.readBytesUntil('\t', saddrstr, 17);
            file.readBytesUntil('\n', sname, 25);
            if (getSensorIndex(saddrstr) > -1) {
                strcpy(sensname[getSensorIndex(saddrstr)],sname);
            }
        }
        file.close();
    }

}
/* ------------------------------------------------------------------------------- */
void setup() {
    pinMode(APREQUEST, INPUT_PULLUP);
    pinMode(PIN_D0, OUTPUT); // pin D0 and D4 are onboard leds on ESP12 boards
    pinMode(PIN_D4, OUTPUT);

    Serial.begin(115200);
    Serial.println("\n\nesp8266 1wire to InfluxDB");

    sensors.begin();
    delay(100);
    
    scount = sensors.getDeviceCount();
    if (scount > MAX_SENSORS) {scount = MAX_SENSORS;}
    for(int i = 0 ; i < scount; i++) {
        if (sensors.getAddress(sensor[i], i)) {
           Serial.printf("Found sensor %d: ",i);
           for (uint8_t j = 0; j < 8; j++) {
                Serial.printf("%02X", sensor[i][j]);
           }
           Serial.printf("\n");
        }
    }
      
    SPIFFS.begin();

    loadSavedSensors();
    
    int len;
    if (SPIFFS.exists("/last_wifi.txt")) {
        file = SPIFFS.open("/last_wifi.txt", "r");
        file.readBytesUntil('\n', lastssid, 33);
        if (lastssid[strlen(lastssid)-1] == 13) {lastssid[strlen(lastssid)-1] = 0;}
        file.readBytesUntil('\n', lastip, 16);
        if (lastip[strlen(lastip)-1] == 13) {lastip[strlen(lastip)-1] = 0;}
        file.close();
    } else {
        strcpy(lastssid, "none");
        strcpy(lastip, "none");
    }
    if (SPIFFS.exists("/influxdb.txt")) {
        file = SPIFFS.open("/influxdb.txt", "r");
        file.readBytesUntil('\n', url, 128);
        if (url[strlen(url)-1] == 13) {url[strlen(url)-1] = 0;}
        file.readBytesUntil('\n', userpass, 64);
        if (userpass[strlen(userpass)-1] == 13) {userpass[strlen(userpass)-1] = 0;}
        file.readBytesUntil('\n', measurement, 32);
        if (measurement[strlen(measurement)-1] == 13) {measurement[strlen(measurement)-1] = 0;}
        file.readBytesUntil('\n', intervalstr, 8);
        if (intervalstr[strlen(intervalstr)-1] == 13) {intervalstr[strlen(intervalstr)-1] = 0;}
        file.close();
        interval = atoi(intervalstr)*60000;
        b64pass = String(userpass);
        b64pass.trim();
        b64pass = base64::encode(b64pass, 0);
    }

    if (SPIFFS.exists("/known_wifis.txt")) {
        char ssid[33];
        char pass[65];
        WiFi.mode(WIFI_STA);  
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(ssid,'\0',sizeof(ssid));
            memset(pass,'\0',sizeof(pass));
            file.readBytesUntil('\t', ssid, 32);
            file.readBytesUntil('\n', pass, 64);
            WiFiMulti.addAP(ssid, pass);
            Serial.printf("wifi loaded: %s / %s\n",ssid,pass);
        }
        file.close();
        httpsclient.setInsecure(); // For allowing self signed, don't care certificate validity
        httpsclient.setTimeout(API_TIMEOUT);
        httpclient.setTimeout(API_TIMEOUT);
    } else {
        startPortal(); // no settings were found, so start the portal without button
    }
    // handle the InfluxDB url
    if (url[0] == 'h') {
        split_url(&urlp, url);
                
        Serial.printf("scheme %s\nhost %s\nport %d\npath %s\n\n",urlp.scheme, urlp.hostn, urlp.port, urlp.path);

        strcpy(scheme, urlp.scheme);
        strcpy(host, urlp.hostn);
        port = urlp.port;
        strcpy(path, urlp.path);
        sprintf(url,"%s://%s:%d%s\0",scheme,host,port,path);
    }
}

/* ------------------------------------------------------------------------------- */
void loop() {
  if (WiFi.getMode() == WIFI_STA) {
      digitalWrite(PIN_D0, HIGH); // LEDs off
      digitalWrite(PIN_D4, HIGH);
      
      char foo[64];
      if (WiFiMulti.run() != WL_CONNECTED) {
          currssid[0] = '\0';
          delay(1000);
      } else {
          WiFi.SSID().toCharArray(foo,64);
          if (strcmp(currssid, foo) != 0) {
              strcpy(currssid, foo);
          }
          WiFi.localIP().toString().toCharArray(foo,64);
          if (strcmp(currip, foo) != 0) {
              strcpy(currip, foo);
          }
          // if our connection has changed, save last wifi info
          if (strcmp(currip, lastip) != 0 || strcmp(currssid, lastssid) != 0) {
              strcpy(lastip, currip);
              strcpy(lastssid, currssid);
              file = SPIFFS.open("/last_wifi.txt", "w");
              file.printf("%s\n%s\n", lastssid, lastip);
              file.close();
          }
      }

      sread = 0;
    
      // If it has been more than interval seconds since last temperature read, do it now
      if ((millis() - mytime > interval && interval > 0) || time == 0) {
          mytime = millis();
          sensors.setWaitForConversion(false);
          sensors.requestTemperatures();
          pinMode(ONE_WIRE_BUS, OUTPUT);
          digitalWrite(ONE_WIRE_BUS, HIGH);
          onewire_wait = 1;
      }
      // after 1000ms per sensor the sensors should be read already
      if (millis() - mytime > 1000*scount && onewire_wait == 1) {
          onewire_wait = 0;
          for (int i = 0; i < scount; i++) {
              if (sensors.isConnected(sensor[i])) {
                  sens[i] = sensors.getTempC(sensor[i]);
              } else {
                  sens[i] = NULL;
              }
          }
          sread = 1;
    
          for (int i = 0; i < scount; i++) {
               sprintf(temp,"%.1f",sens[i]);
               if (temp[0] != 0) {
                   Serial.printf("sensor %d %s°C\n",i, temp);
               }
          }
      }
      if (sread == 1) {
          if ((WiFiMulti.run() == WL_CONNECTED)) {
              Serial.print("connected as ");
              Serial.print(WiFi.localIP());
              Serial.print(" from WiFi ");
              Serial.println(WiFi.SSID());
              Serial.println(url);
              Serial.printf("connecting to %s:%d\n", host, port);
              Serial.printf("POST %s\npost-data: ", path);

              memset(postmsg,0,sizeof(postmsg));
              sprintf(postmsg,"%s ",measurement);
              for (uint8_t i = 0; i < MAX_SENSORS; i++) {
                  if (strlen(sensname[i]) > 0 && sensors.isConnected(sensor[i])) {
                      memset(foo,0,sizeof(foo));
                      sprintf(foo,"%s=%.1f,",sensname[i],sens[i]);
                      strcat(postmsg, foo);
                  }
              }
              postmsg[strlen(postmsg)-1] = 0; // remove last comma
              Serial.printf("%s\n\n",postmsg);
                            
              digitalWrite(PIN_D0, LOW); // leds on when in connect
              digitalWrite(PIN_D4, LOW);

              if (strcmp(scheme, "https") == 0) {
                  if (httpsclient.connect(host, port)) {
                      httpsclient.printf("POST %s HTTP/1.1\nHost: %s\n", path, host);
                      httpsclient.printf("Content-Length: %d\nAuthorization: Basic ", strlen(postmsg));
                      httpsclient.print(b64pass);
                      httpsclient.print("\nConnection: close\n\n");
                      httpsclient.print(postmsg);
                
                      while (httpsclient.connected()) {
                          Serial.println(httpsclient.readString());
                      }
                      httpsclient.stop();
                  } else {
                      Serial.printf("%s connect failed.\n",scheme);
                  }
              }
              if (strcmp(scheme, "http") == 0) {
                  if (httpclient.connect(host, port)) {
                      httpclient.printf("POST %s HTTP/1.1\nHost: %s\n", path, host);
                      httpclient.printf("Content-Length: %d\nAuthorization: Basic ", strlen(postmsg));
                      httpclient.print(b64pass);
                      httpclient.print("\nConnection: close\n\n");
                      httpclient.print(postmsg);
                
                      while (httpclient.connected()) {
                          Serial.println(httpclient.readString());
                      }
                      httpclient.stop();
                  } else {
                      Serial.printf("%s connect failed.\n",scheme);
                  }
              }              
              digitalWrite(PIN_D0, HIGH); // leds off
              digitalWrite(PIN_D4, HIGH);
          }
      }
  } else if (WiFi.getMode() == WIFI_AP) { // portal mode
      dnsServer.processNextRequest();
      server.handleClient();

      // blink onboard leds if we are in portal mode
      if (int(millis()%1000) < 500) {
          digitalWrite(PIN_D0, LOW);
          digitalWrite(PIN_D4, HIGH);
      } else {
          digitalWrite(PIN_D0, HIGH);
          digitalWrite(PIN_D4, LOW);
      }
  }
  if (digitalRead(APREQUEST) == LOW && WiFi.getMode() == WIFI_STA) {
      startPortal();
  }
  if (millis() - portal_timer > APTIMEOUT && WiFi.getMode() == WIFI_AP) {
      Serial.println("Portal timeout. Booting.");
      delay(1000);
      ESP.restart();
  }
}
/* ------------------------------------------------------------------------------- */
/* Portal code begins here
 *  
 */
/* ------------------------------------------------------------------------------- */

void startPortal() {
    portal_timer = millis();
    WiFi.disconnect();
    delay(100);
    
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("ESP8266 1WIRE");
    
    dnsServer.setTTL(300);
    dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
    dnsServer.start(53, "*", apIP);
    
    server.on("/", httpRoot);
    server.on("/style.css", httpStyle);
    server.on("/influx.html", httpInflux);
    server.on("/saveinfl", httpSaveInflux);
    server.on("/wifis.html", httpWifis);
    server.on("/savewifi", httpSaveWifi);
    server.on("/sensors.html", httpSensors);
    server.on("/savesens",httpSaveSensors);
    server.on("/boot", httpBoot);
    
    server.onNotFound([]() {
        server.sendHeader("Refresh", "1;url=/"); 
        server.send(404, "text/plain", "QSD QSY");
    });
    server.begin();
    Serial.println("Started portal");    
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/index.html", "r");
    html = file.readString();
    file.close();    
    html.replace("###LASTSSID###", lastssid);
    html.replace("###LASTIP###", lastip);
    
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpInflux() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/influx.html", "r");
    html = file.readString();
    file.close();
    
    html.replace("###URL###", String(url));
    html.replace("###USERPASS###", String(userpass));
    html.replace("###IDBM###", String(measurement));
    html.replace("###INTERVAL###", String(intervalstr));
        
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveInflux() {
    portal_timer = millis();
    String html;
        
    file = SPIFFS.open("/influxdb.txt", "w");
    file.println(server.arg("url"));
    file.println(server.arg("userpass"));
    file.println(server.arg("idbm"));
    file.println(server.arg("interval"));
    file.close();

    // reread
    file = SPIFFS.open("/influxdb.txt", "r");
    file.readBytesUntil('\n', url, 128);
    if (url[strlen(url)-1] == 13) {url[strlen(url)-1] = 0;}
    file.readBytesUntil('\n', userpass, 64);
    if (userpass[strlen(userpass)-1] == 13) {userpass[strlen(userpass)-1] = 0;}
    file.readBytesUntil('\n', measurement, 32);
    if (measurement[strlen(measurement)-1] == 13) {measurement[strlen(measurement)-1] = 0;}
    file.readBytesUntil('\n', intervalstr, 8);
    if (intervalstr[strlen(intervalstr)-1] == 13) {intervalstr[strlen(intervalstr)-1] = 0;}
    file.close();

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "3;url=/");
    server.send(200, "text/html; charset=UTF-8", html);    
}
/* ------------------------------------------------------------------------------- */

void httpWifis() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    char ssid[33];
    char pass[33];
    int counter = 0;
    
    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));
    
    file = SPIFFS.open("/wifis.html", "r");
    html = file.readString();
    file.close();
    
    if (SPIFFS.exists("/known_wifis.txt")) {
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(rowbuf, '\0', sizeof(rowbuf)); 
            memset(ssid, '\0', sizeof(ssid));
            memset(pass, '\0', sizeof(pass));
            file.readBytesUntil('\t', ssid, 33);
            file.readBytesUntil('\n', pass, 33);
            sprintf(rowbuf,"<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>",counter,ssid);
            strcat(tablerows,rowbuf);
            sprintf(rowbuf,"<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"32\" value=\"%s\"></td></tr>",counter,pass);
            strcat(tablerows,rowbuf);
            counter++;
        }
        file.close();
    }
    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));
    
    if (counter > 3) {
        html.replace("table-row", "none");
    }
    
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
    portal_timer = millis();
    String html;
        
    file = SPIFFS.open("/known_wifis.txt", "w");
    
    for (int i = 0; i < server.arg("counter").toInt(); i++) {
         if (server.arg("ssid"+String(i)).length() > 0) {
             file.print(server.arg("ssid"+String(i)));
             file.print("\t");
             file.print(server.arg("pass"+String(i)));
             file.print("\n");
         }
    }
    // Add new
    if (server.arg("ssid").length() > 0) {
        file.print(server.arg("ssid"));
        file.print("\t");
        file.print(server.arg("pass"));
        file.print("\n");
    }    
    file.close();

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "3;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSensors() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    char sname[25];
    char saddrstr[17];
    char tmp[4];
    int counter = 0;
    uint8_t unexistents = 0;
    DeviceAddress saddr;
    
    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));
    memset(tmp, '\0', sizeof(tmp));
    
    file = SPIFFS.open("/sensors.html", "r");
    html = file.readString();
    file.close();
        
    for(int i = 0 ; i < scount; i++) {
        memset(saddrstr, '\0', sizeof(saddrstr));
        memset(sname, '\0', sizeof(sname));
        for (uint8_t j = 0; j < 8; j++) {
            sprintf(tmp,"%02X",sensor[i][j]);
            strcat(saddrstr,tmp); tmp[2] = 0;
            sprintf(tmp,"%02X:",sensor[i][j]);
            strcat(sname,tmp); tmp[3] = 0;
        }
        if (saddrstr[0] != 0) {
            sname[strlen(sname)-1] = 0;
            sprintf(rowbuf,"<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">",sname,i,sensname[i]);
            strcat(tablerows,rowbuf);
            sprintf(rowbuf,"<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>",i,saddrstr);
            strcat(tablerows,rowbuf);
            counter++;
        }
    }
    if (SPIFFS.exists("/known_sensors.txt")) {
        file = SPIFFS.open("/known_sensors.txt", "r");
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(saddrstr, '\0', sizeof(saddrstr));
            
            file.readBytesUntil('\t', saddrstr, 17);
            file.readBytesUntil('\n', sname, 25);
            if (getSensorIndex(saddrstr) == -1) {
                if (saddrstr[0] != 0) {
                    if (unexistents == 0) {
                        unexistents = 1;
                        strcat(tablerows,"<tr><td><hr /><b>Unexistent but saved sensors</b></td></tr>");
                    }
                    if (sname[strlen(sname)-1] == 13) {sname[strlen(sname)-1] = 0;}
                    sprintf(rowbuf,"<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">",saddrstr,counter,sname);
                    strcat(tablerows,rowbuf);
                    sprintf(rowbuf,"<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>",counter,saddrstr);
                    strcat(tablerows,rowbuf);
                    counter++;
                }
            }
        }
        file.close();
    }

    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));
            
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveSensors() {
    portal_timer = millis();
    String html;
        
    file = SPIFFS.open("/known_sensors.txt", "w");
    
    for (int i = 0; i < server.arg("counter").toInt(); i++) {
         if (server.arg("sname"+String(i)).length() > 0) {
             file.print(server.arg("saddr"+String(i)));
             file.print("\t");
             file.print(server.arg("sname"+String(i)));
             file.print("\n");
         }
    }
    file.close();
    loadSavedSensors(); // reread

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "3;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpStyle() {
    portal_timer = millis();
    String css;
        
    file = SPIFFS.open("/style.css", "r");
    css = file.readString();
    file.close();       
    server.send(200, "text/css", css);
}
/* ------------------------------------------------------------------------------- */

void httpBoot() {
    portal_timer = millis();
    String html;
    
    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();
    
    server.sendHeader("Refresh", "3;url=about:blank");
    server.send(200, "text/html; charset=UTF-8", html);
    delay(1000);
    ESP.restart();
}
/* ------------------------------------------------------------------------------- */
