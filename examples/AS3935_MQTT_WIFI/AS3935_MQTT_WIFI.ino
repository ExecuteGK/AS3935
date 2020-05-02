#include "FS.h"
#include "esp_system.h"
#include <esp_wifi.h>
#include <string.h>
#include <WiFi.h>
#include <Preferences.h>  // WiFi storage
#include <AsyncDelay.h>
#include <SoftWire.h>
#include <AS3935.h>
#include <PubSubClient.h>

  AS3935 as3935;
  AsyncDelay d;
  AsyncDelay NF;
  uint8_t AS3935_NoiseFloor = 0;
  uint8_t AS3935_MaxNoiseFloor = 7;
  int NFReductionDelay = 120000;
  const boolean retained = true;

  const  char* rssiSSID;       // NO MORE hard coded set AP, all SmartConfig
  const  char* password;
  String PrefSSID, PrefPassword;  // used by preferences storage

  String mqtt_server;

  //Create webServer for config
  WiFiServer server(80);
  String header;

  const char* PARAM_INPUT_1 = "input1";
  
  WiFiClient espClient;
  PubSubClient client(espClient);
  long lastMsg = 0;
  char msg[50];
  int value = 0;

  int  WFstatus;
  int UpCount = 0;
  int32_t rssi;           // store WiFi signal strength here
  String getSsid;
  String getPass;
  String  MAC;

  // SSID storage
       Preferences preferences;  // declare class object
  // END SSID storage

void int2Handler(void)
{
  as3935.interruptHandler();
}

void readRegs(uint8_t start, uint8_t end)
{
  for (uint8_t reg = start; reg < end; ++reg) {
    delay(50);
    uint8_t val;
    as3935.readRegister(reg, val);

    Serial.print("Reg: 0x");
    Serial.print(reg, HEX);
    Serial.print(": 0x");
    Serial.println(val, HEX);
    Serial.flush();
  }
  Serial.print("State: ");
  Serial.println(as3935.getState(), DEC);

  Serial.println("-------------");
}

void printInterruptReason(Stream &s, uint8_t value, const char *prefix = nullptr)
{
  if (value & AS3935::intNoiseLevelTooHigh) {
    if (prefix)
      s.print(prefix);
    s.println(F("Noise level too high"));
    AS3935_NoiseFloor++;
    if (AS3935_NoiseFloor <= AS3935_MaxNoiseFloor) {
      as3935.setNoiseFloor(AS3935_NoiseFloor);
      NF.start(NFReductionDelay, AsyncDelay::MILLIS);
      
      String pubString = String(AS3935_NoiseFloor);
      pubString.toCharArray(msg,pubString.length()+1);
      client.publish("as3935/NoiseFloorLevel", msg);
    } else {
      AS3935_NoiseFloor = AS3935_MaxNoiseFloor;
      NF.start(NFReductionDelay, AsyncDelay::MILLIS);
    }
  }
  if (value & AS3935::intDisturberDetected) {
    if (prefix)
      s.print(prefix);
    s.println(F("Disturber detected"));
    client.publish("as3935/DisturberDetected", "true");
  }
  if (value & AS3935::intLightningDetected) {
    if (prefix)
      s.print(prefix);
    s.println(F("Lightning detected"));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\tWiFi Setup -- \n"  ); 

  wifiInit();       // get WiFi connected
  IP_info();
  MAC = getMacAddress();

  delay(2000);

  Serial.printf("\n\Webserver Setup -- \n"  ); 
  //start wifi server
  server.begin();
  
  Serial.printf("\n\MQTT Setup -- \n"  ); 

  // Check if MQTT server has been stored in preferences store
  // This prepares for a later web config tool
  preferences.begin("mqtt", false);
      mqtt_server =  preferences.getString("mqtt", "flowcontrol-wifi.lan");      //NVS key mqtt
  preferences.end();

  char *cmqtt = new char[mqtt_server.length()+1];
  strcpy(cmqtt, mqtt_server.c_str());
  
  client.setServer(cmqtt, 1883);
  client.setCallback(callback);

  Serial.printf("\n\tAS3935 Setup -- \n"  ); 
  as3935.initialise(21, 22, 0x03, 3, true, NULL);

  Serial.printf("\n\tAS3935 Calibration -- \n"  ); 
  attachInterrupt(19, int2Handler, RISING);
  as3935.calibrate();
  as3935.start();
  d.start(1000, AsyncDelay::MILLIS);

  while (!d.isExpired())
    as3935.process();

  readRegs(0, 0x09);
  Serial.printf("\n\tAS3935 Set NoiseFloor and supress Disturber -- \n"  ); 
  as3935.setNoiseFloor(AS3935_NoiseFloor);
  as3935.setRegisterBit(3,5,true);
  Serial.println("AS3935 Setup complete");
} //  END setup()

void callback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Feel free to add more if statements to control more with MQTT

  // If a message is received on the topic as3935/ReportDisturber, you check if the message is either "true" or "false". 
  // Changes the reporting of Disturbers
  if (String(topic) == "as3935/ReportDisturber") {
    Serial.print("Changing Disturber Report to ");
    if(messageTemp == "false"){
      Serial.println("false");
      as3935.setRegisterBit(3,5,true);
    }
    else if(messageTemp == "true"){
      Serial.println("true");
      as3935.setRegisterBit(3,5,false);
    }
    readRegs(0, 0x09);
  }
}

void reconnect() {
  // No Loop until we're reconnected, since the config server needs to run
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP32Client")) {
      Serial.println("connected");
      // Subscribe
      client.subscribe("as3935/ReportDisturber");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
    }
}
  
  
void loop()
{
  if ( WiFi.status() ==  WL_CONNECTED )      // Main connected loop

  { 
    // ANY MAIN LOOP CODE HERE


    //NoiseFloor handling
    //Executes in order to reduce the NoiseFloorLevel in order to 
    //keep it as low as possible
    
    if (NF.isExpired()){
      if (AS3935_NoiseFloor > 0) {
        AS3935_NoiseFloor--;
        as3935.setNoiseFloor(AS3935_NoiseFloor);
        NF.start(NFReductionDelay, AsyncDelay::MILLIS);
        
        String pubString = String(AS3935_NoiseFloor);
        pubString.toCharArray(msg,pubString.length()+1);
        client.publish("as3935/NoiseFloorLevel", msg);
      }
    }

    //Check MQTT Connection and publish initial message
    
    if (!client.connected()) {
      reconnect();
      if (client.connected()) {
        client.publish("as3935/connected", "true");
      
        String pubString = String(AS3935_NoiseFloor);
        pubString.toCharArray(msg,pubString.length()+1);
        client.publish("as3935/NoiseFloorLevel", msg);
      }
    }
    client.loop();

    //Process as3935 task and check for lightnings
    
    if (as3935.process()) {
      uint8_t flags = as3935.getInterruptFlags();
      uint8_t dist = as3935.getDistance();

      Serial.println("-------------------");
      Serial.println("Interrupt!");
      Serial.println("Reason(s):");
      printInterruptReason(Serial, flags, "    ");
      if (AS3935::intLightningDetected & flags) {
        Serial.print("Distance: ");
        Serial.println(dist, DEC);
        client.publish("as3935/lightning", "true");
        String pubString = String(dist);
        pubString.toCharArray(msg,pubString.length()+1);
        client.publish("as3935/lastdistance", msg);
      }
    }

    if (as3935.getBusError()) {
      Serial.println("Bus error!");
      as3935.clearBusError();
    }

    //Check for config page requests via http
    //this is the config server allowing the configuration of the MQTT server

    WiFiClient webconfig = server.available();  // Listen for incoming clients

    if (webconfig) {                            // If a new client connects,  
      Serial.println("New Client.");          // print a message out in the serial port
      String currentLine = "";                // make a String to hold incoming data from the client
      while (webconfig.connected()) {            // loop while the client's connected
        if (webconfig.available()) {             // if there's bytes to read from the client,
          char c = webconfig.read();             // read a byte, then
          Serial.write(c);                    // print it out the serial monitor
          header += c;
          if (c == '\n') {                    // if the byte is a newline character
                                              // if the current line is blank, you got two newline characters in a row.
                                              // that's the end of the client HTTP request, so send a response:
            if (currentLine.length() == 0) {
                                              // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
                                              // and a content-type so the client knows what's coming, then a blank line:
              webconfig.println("HTTP/1.1 200 OK");
              webconfig.println("Content-type:text/html");
              webconfig.println("Connection: close");
              webconfig.println();
            
              
                                              // Display the HTML web page
              webconfig.println("<!DOCTYPE html><html>");
              webconfig.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
              webconfig.println("<link rel=\"icon\" href=\"data:,\">");
                                              // CSS to style the on/off buttons 
                                              // Feel free to change the background-color and font-size attributes to fit your preferences
              webconfig.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
              webconfig.println(".button { background-color: #4CAF50; border: none; color: white; padding: 16px 40px;");
              webconfig.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
              webconfig.println(".button2 {background-color: #555555;}</style></head>");
            
                                              // Web Page Heading
              webconfig.println("<body><h1>AS3935 Web Server</h1>");
              webconfig.println("<form action='get'>");
              webconfig.println("MQTT Server: <input type='text' name='input1'>");
              webconfig.println("<input type='submit' value='Submit'>");
              webconfig.println("</form><br>");
              webconfig.println("");
              webconfig.println("");
              webconfig.println("");
              webconfig.println("");
              webconfig.println("");
              webconfig.println("");
              webconfig.println("</body></html>");
            
                                              // The HTTP response ends with another blank line
              webconfig.println();
                                              // Break out of the while loop
              break;
            } else { // if you got a newline, then clear currentLine
              currentLine = "";
            }
          } else if (c != '\r') {  // if you got anything else but a carriage return character,
            currentLine += c;      // add it to the end of the currentLine
          }
      }
    }
    // Clear the header variable
    header = "";
    // Close the connection
    webconfig.stop();
    Serial.println("Client disconnected.");
    Serial.println("");
  }
    
    
  }   // END Main connected loop()
  else
  {            // WiFi DOWN

     //  wifi down start LED flasher here

         WFstatus = getWifiStatus( WFstatus );

     WiFi.begin( PrefSSID.c_str() , PrefPassword.c_str() );
     int WLcount = 0;
     while (WiFi.status() != WL_CONNECTED && WLcount < 200 )
     {
      delay( 100 );
         Serial.printf(".");

         if (UpCount >= 60)  // keep from scrolling sideways forever
         {
            UpCount = 0;
               Serial.printf("\n");
         }
         ++UpCount;
         ++WLcount;
     }

    if( getWifiStatus( WFstatus ) == 3 )   //wifi returns
    { 
    // stop LED flasher, wifi going up
    }
     delay( 1000 );
  }  // END WiFi down 
} // END loop()
  
void wifiInit()  // 
{
   WiFi.mode(WIFI_AP_STA);   // required to read NVR before WiFi.begin()

   // load credentials from NVR, a little RTOS code here
   wifi_config_t conf;
   esp_wifi_get_config(WIFI_IF_STA, &conf);  // load wifi settings to struct comf
   rssiSSID = reinterpret_cast<const char*>(conf.sta.ssid);
   password = reinterpret_cast<const char*>(conf.sta.password);

    //  Serial.printf( "%s\n", rssiSSID );
    //  Serial.printf( "%s\n", password );

   // Open Preferences with "wifi" namespace. Namespace is limited to 15 chars
   preferences.begin("wifi", false);
       PrefSSID          =  preferences.getString("ssid", "none");      //NVS key ssid
       PrefPassword  =  preferences.getString("password", "none");  //NVS key password
   preferences.end();

   // keep from rewriting flash if not needed
   if( !checkPrefsStore() )     // see is NV and Prefs are the same
   {              // not the same, setup with SmartConfig
      if( PrefSSID == "none" )  // New...setup wifi
      {
        initSmartConfig(); 
        delay( 3000);
        ESP.restart();   // reboot with wifi configured
      }
   } 

   // I flash LEDs while connecting here

   WiFi.begin( PrefSSID.c_str() , PrefPassword.c_str() );

   int WLcount = 0;
   while (WiFi.status() != WL_CONNECTED && WLcount < 200 ) // can take > 100 loops depending on router settings
   {
     delay( 100 );
        Serial.printf(".");
     ++WLcount;
   }
  delay( 3000 );

  //  stop the led flasher here

  }  // END wifiInit()

// match WiFi IDs in NVS to Pref store,  assumes WiFi.mode(WIFI_AP_STA);  was executed
bool checkPrefsStore()   
{
    bool val = false;
    String NVssid, NVpass, prefssid, prefpass;

    NVssid = getSsidPass( "ssid" );
    NVpass = getSsidPass( "pass" );

    // Open Preferences with my-app namespace. Namespace name is limited to 15 chars
    preferences.begin("wifi", false);
        prefssid  =  preferences.getString("ssid", "none");     //NVS key ssid
        prefpass  =  preferences.getString("password", "none"); //NVS key password
    preferences.end();

    if( NVssid.equals(prefssid) && NVpass.equals(prefpass) )
      { val = true; }

  return val;
}

// optionally call this function any way you want in your own code
// to remap WiFi to another AP using SmartConfig mode.   Button, condition etc.. 
void initSmartConfig() 
{
   // start LED flasher
  int loopCounter = 0;

  WiFi.mode( WIFI_AP_STA );       //Init WiFi, start SmartConfig
      Serial.printf( "Entering SmartConfig\n" );

  WiFi.beginSmartConfig();

  while (!WiFi.smartConfigDone()) 
  {
     // flash led to indicate not configured
          Serial.printf( "." );
     if( loopCounter >= 40 )  // keep from scrolling sideways forever
     {
         loopCounter = 0;
         Serial.printf( "\n" );
     }
     delay(600);
    ++loopCounter;
  }
  loopCounter = 0;

  // stopped flasher here

   Serial.printf("\nSmartConfig received.\n Waiting for WiFi\n\n");
   delay(2000 );
    
  while( WiFi.status() != WL_CONNECTED )      // check till connected
  { 
    delay(500);
  }
  IP_info();  // connected lets see IP info

  preferences.begin("wifi", false);      // put it in storage
     preferences.putString( "ssid"         , getSsid);
     preferences.putString( "password", getPass);
  preferences.end();

    delay(300);
}  // END SmartConfig()

void IP_info()
{
   getSsid = WiFi.SSID();
   getPass = WiFi.psk();
   rssi = getRSSI(  rssiSSID );
   WFstatus = getWifiStatus( WFstatus );
   MAC = getMacAddress();

      Serial.printf( "\n\n\tSSID\t%s, ", getSsid.c_str() );
      Serial.print( rssi);  Serial.printf(" dBm\n" );  // printf??
      Serial.printf( "\tPass:\t %s\n", getPass.c_str() ); 
      Serial.print( "\n\n\tIP address:\t" );  Serial.print(WiFi.localIP() );
      Serial.print( " / " );
      Serial.println( WiFi.subnetMask() );
      Serial.print( "\tGateway IP:\t" );  Serial.println( WiFi.gatewayIP() );
      Serial.print( "\t1st DNS:\t" );     Serial.println( WiFi.dnsIP() );
      Serial.printf( "\tMAC:\t\t%s\n", MAC.c_str() );
}

int getWifiStatus( int WiFiStatus  )
{
  WiFiStatus = WiFi.status();
  Serial.printf("\tStatus %d",  WiFiStatus );
  switch( WiFiStatus )
  {
    case WL_IDLE_STATUS :                         // WL_IDLE_STATUS     = 0,
          Serial.printf(", WiFi IDLE \n");
          break;
    case WL_NO_SSID_AVAIL:                        // WL_NO_SSID_AVAIL   = 1,
          Serial.printf(", NO SSID AVAIL \n");
          break;
    case WL_SCAN_COMPLETED:                       // WL_SCAN_COMPLETED  = 2,
          Serial.printf(", WiFi SCAN_COMPLETED \n");
          break;
    case WL_CONNECTED:                            // WL_CONNECTED       = 3,
          Serial.printf(", WiFi CONNECTED \n");
          break;
    case WL_CONNECT_FAILED:                       // WL_CONNECT_FAILED  = 4,
          Serial.printf(", WiFi WL_CONNECT FAILED\n"); 
          break;
    case WL_CONNECTION_LOST:                      // WL_CONNECTION_LOST = 5,
          Serial.printf(", WiFi CONNECTION LOST\n");
          WiFi.persistent(false);                 // don't write FLASH
          break;
    case WL_DISCONNECTED:                         // WL_DISCONNECTED    = 6
          Serial.printf(", WiFi DISCONNECTED ==\n");
          WiFi.persistent(false);                 // don't write FLASH when reconnecting
          break;
  }
  return WiFiStatus;
}
// END getWifiStatus()

// Get the station interface MAC address.
// @return String MAC
String getMacAddress(void)
{
    WiFi.mode(WIFI_AP_STA);                    // required to read NVR before WiFi.begin()
    uint8_t baseMac[6];
    esp_read_mac( baseMac, ESP_MAC_WIFI_STA ); // Get MAC address for WiFi station
    char macStr[18] = { 0 };
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
    return String(macStr);
}
// END getMacAddress()


// Return RSSI or 0 if target SSID not found
// const char* SSID = "YOUR_SSID";  // declare in GLOBAL space
// call:  int32_t rssi = getRSSI( SSID );
int32_t getRSSI( const char* target_ssid ) 
{
  byte available_networks = WiFi.scanNetworks();

  for (int network = 0; network < available_networks; network++) 
  {
    if ( strcmp(  WiFi.SSID( network).c_str(), target_ssid ) == 0) 
    {
      return WiFi.RSSI( network );
    }
  }
  return 0;
} //  END  getRSSI()


// Requires; #include <esp_wifi.h>
// Returns String NONE, ssid or pass arcording to request 
// ie String var = getSsidPass( "pass" );
String getSsidPass( String s )
{
  String val = "NONE";  // return "NONE" if wrong key sent
  s.toUpperCase();
  if( s.compareTo("SSID") == 0 )
  {
     wifi_config_t conf;
     esp_wifi_get_config( WIFI_IF_STA, &conf );
     val = String( reinterpret_cast<const char*>(conf.sta.ssid) );
  }
  if( s.compareTo("PASS") == 0 )
  {
     wifi_config_t conf;
     esp_wifi_get_config( WIFI_IF_STA, &conf );
     val = String( reinterpret_cast<const char*>(conf.sta.password) );
  }
 return val;
}
