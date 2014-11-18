#include "Arduino.h"
#include <SipHash_2_4.h>

#define R1_DET 20
#define LOGGING_ENABLE false
#define SHOW_RAW false


unsigned long timesent;
unsigned long logts;

byte rfidp[256];
byte rf_i;
byte response[256];

//rr10
byte command[256];
boolean incmd = false;
byte comstatus; // 0 : idle, 1 : handshake sent , 2 : command sent , 3 : handshake sent back, waiting for answer
int card_type = 0;
byte iso14443_null[6] = { 0x06, 0x09, 0x00, 0x00, 0x0F, 0x00 };
byte iso15693_null[5] = { 0x05, 0x06, 0x00, 0x0B, 0x00 };
byte null_response[9];
boolean received = false;

// sipHash
unsigned char key[] = { 0xe8, 0x0b, 0x6e, 0x3a, 0x12, 0x11, 0x40, 0x57,
                        0x7c, 0x7b, 0xea, 0x17, 0x64, 0x08, 0xe8, 0x6e };

void setup()
{
  Serial.begin(9600);
  
  pinMode(R1_DET, INPUT);
  Serial1.begin(115200);

}

void loop()
{
  switch (card_type) {
    case 0: {
      // ISO14443
      // command 0x09 : ISO14443 Tag Inventory
      
      byte cmd[5] = {0x05,0x09,0x00,0x00,0x00};
      memcpy(null_response, iso14443_null, sizeof iso14443_null);
      sendCmd(cmd);
      while (!cmdUpdate()) {
        // log("sending cmd to look for ISO14443");
      }

      if (received) {
        
        // Serial.print("number of tags: ");
        // Serial.println(rfidp[3]);

        int uidlen = rfidp[4];
        byte uid[uidlen];

        // Serial.print("uid length: ");
        // Serial.println(uidlen);

        byte sip_uid[8];
        
        sipHash.initFromRAM(key);
        for(int i=0;i<uidlen;i++)
        {
          uid[i] = rfidp[12+i];
          sipHash.updateHash((byte)uid[i]);
        }
        sipHash.finish();

        sip_uid[0] = 0xE0;
        sip_uid[1] = 0x04;

        for(int i = 2; i < 8; i++) {
          sip_uid[i] = (byte)sipHash.result[i];
        }

        Serial.print("uid: ");
        Serial.println(printhex(uid, uidlen));

        Serial.print("sipHash: ");
        Serial.println(printhex(sip_uid, 8));

      }
      

      card_type = 1;
      break;
    }
    case 1: {
      // ISO15693
      // command 0x06 : ISO15693 Tag Inventory (params : normal mode, no AFI)

      byte cmd[7] = {0x07,0x06,0x00,0x00,0x00};
      memcpy(null_response, iso15693_null, sizeof iso15693_null);
      sendCmd(cmd);

      while (!cmdUpdate()) {
        // log("sending cmd to look for ISO15693");
      }

      if (received) {
        if(rfidp[2] != 0) {
          int uidlen = 8;
          byte uid[uidlen];
          //at least one tag is found, let's read the uid of the first tag (extra tags are ignored)
          for(int i=0;i<8;i++) {
             uid[i] = rfidp[11-i];
          }
          Serial.print("uid length: ");
          Serial.println(uidlen);

          Serial.print("uid: ");
          Serial.println(printhex(uid, uidlen));
        }
      }

      card_type = 0;
      break;
    }
    // module version test
    // case 2: {
    //   byte cmd[6] = {0x06,0x01,0x05,0x0a};
    //   sendCmd(cmd);
    //   while (!cmdUpdate()) {
    //   }
    //   break;
    // }
  }

}

boolean cmdUpdate() {
  // determine which part of the command sequence we're in
  // pg45 http://www.reyax.com/Module/RFID/RR10_Programming_Guide_V2.0.pdf
  // it's a picture
  switch (comstatus) {
    case 0:
      log("sending handshake");
      rf_i = 0;
      Serial1.write(0x55);
      timesent = millis();
      received = false;
      comstatus = 1;
      break;
    case 1: // wait for response
      if (Serial1.available() == 0) {
        // log("no response (1)");
        break;
      }

      if (Serial1.read() != 0xAA) {
        // log("bad response (1)");
        comstatus = 0;
        break;
      }

      log("received resp, sending command");

      Serial1.write(command, command[0]);
      comstatus = 2; // do nothing for now
      break;
    case 2:
      // wait for handshake request and respond
      if (Serial1.available() == 0) { // no response
        // log("no response (2)");
        break;
      }
      // ensure response is correct
      if (Serial1.read() != 0xA5) {
        // log("bad response (2)");
        comstatus = 0;
        break;
      }
      log("completing handshake");
      Serial1.write(0x5A);
      comstatus = 3;
      break;
    case 3:
      if (Serial1.available() == 0) {
        // log("no response (3)");
        break;
      }
      // add received data to the buffer
      rfidp[rf_i] = Serial1.read(); 
      rf_i++;

      if(rf_i < rfidp[0]) { //response not fully received yet
        break;
      } 
      { //compute checksum
        word chksm=0;
        for(int i=0;i<rfidp[0]-2;i++)
          chksm += rfidp[i];
    
        if(chksm != ((((word)rfidp[rfidp[0]-1])<<8) + rfidp[rfidp[0]-2]) ) //if checksum mismatch
        {
          comstatus = 0; //let's try again from the beginning
          break;
        }
      }

      boolean empty = true;
      for (int i=0; i < rfidp[0]; i++) {

        if (rfidp[i] != null_response[i]) {
          empty = false;
          break;
        }
      }
      

      if (empty) {
        log("empty response; resetting");
        comstatus = 0;
        incmd = false;
        return true;
      } else {

        if (SHOW_RAW) {
          Serial.print("raw length: ");
          Serial.println(rfidp[0]);
          Serial.print("raw: ");
          Serial.println(printhex(rfidp, rfidp[0]));
        }
        
        comstatus = 0;
        incmd = false;
        received = true;
        return true;
      }
      
  }

  if (millis() - timesent > 1000) {
    Serial.println("timeout");
    comstatus = 0;
    incmd = false;
    return true;
  }
  return false;
}

void sendCmd(byte* cmd)
{
  if (incmd)
    return;

  memcpy(command,cmd,cmd[0] - 2);//store command
  
  //compute checksum
  word chksm = 0;
  for(int i=0;i<command[0]-2;i++)
  chksm += command[i];
  command[command[0]-2]=chksm;
  command[command[0]-1]=chksm>>8;
  
  incmd = true;
  
}

void log(String msg) {
  if (LOGGING_ENABLE) {
    Serial.println(msg);
  }
}

void log_d (String msg) {
  if (millis() - logts > 10000 || logts == 0) {
    Serial.println(msg);
    logts = millis();
  }
}

String printhex(byte* bytes, int len) {
  String result = "";
  for (int i=0; i != len; i++) {
    char sbyte[5];
    sprintf(sbyte, "%02X", bytes[i]);
    result += sbyte;
    // result += " ";
  }
  return result;
}