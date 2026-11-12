#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include "secrets.h" 
#include <ESP32Ping.h>

extern void csiSetup();
extern void csiLoop();

WiFiClientSecure client;
WiFiClient tcpClient;
WiFiUDP udp;

UniversalTelegramBot bot(BOT_TOKEN, client);
unsigned long ultimo_momento_visto = 0;

IPAddress ip_pc(0, 0, 0, 0);
const int pino_led = 2;

unsigned long tuv = 0; 
const unsigned long intervalo_varredura = 5000;

void liga_pc_wol(){
    byte packet_magic[102];
    for (int i = 0; i< 6; i++){
      packet_magic[i] = 0xFF;
    }

    int indice_pacote = 6;

    for (int i = 0; i<16; i++){
      for (int a = 0; a < 6; a++){
        packet_magic[indice_pacote] = MAC_ALVO[a];
        indice_pacote++;
      }
    }

    udp.beginPacket("255.255.255.255",9);
    udp.write(packet_magic,102);
    udp.endPacket();
}

void gerencia_mensagem(int numero_mensagens){
  Serial.print("Carregando...");
  Serial.print(numero_mensagens);
  Serial.println(" mensagens");

  for (int i = 0; i < numero_mensagens; i++){
    String id_remetente = String(bot.messages[i].chat_id);
    String nome_remetente = String(bot.messages[i].from_name);
    String texto = bot.messages[i].text;
    texto.toLowerCase();
    
    if (id_remetente != chatID){
      Serial.printf("Acesso não autorizado\nNome: %s / ID: %s / Texto enviado: %s/", nome_remetente.c_str(), id_remetente.c_str(), texto.c_str());
      bot.sendMessage(id_remetente,"Acesso negado!");
      continue;
    }

    if (texto == "start"){
      String hello_world = "Olá " + nome_remetente + "\n";
      hello_world += "🟢 ligar computador\n";
      hello_world += "🔴 desligar computador\n";
      hello_world += "🔍 status\n";
      hello_world += "🛡️ ativar radar\n";
      hello_world += "🛌 desativar radar\n";

      bot.sendMessage(chatID, hello_world, "");
    }

    else if (texto.indexOf("desativar") != -1 && texto.indexOf("radar") != -1){
      bot.sendMessage(chatID, "🛌 Radar DESARMADO. Sistema em modo silencioso.", "");
      Serial.println("[CMD:ALARME_OFF]"); // O Python vai ler isso
    }


    else if (texto.indexOf("ativar") != -1 && texto.indexOf("radar") != -1){
      bot.sendMessage(chatID, "🛡️ Radar ARMADO! Avisarei sobre movimentos.", "");
      Serial.println("[CMD:ALARME_ON]"); // O Python vai ler isso
    }


    else if (texto.indexOf("status") != -1){
          // Se a ESP32 tem um IP salvo, é porque a porta 5000 está viva
          if (ip_pc[0] != 0) {
            bot.sendMessage(chatID, "Computador ligado!\nIP: " + ip_pc.toString(), "");
          } else {
            bot.sendMessage(chatID, "Sem resposta do computador", "");
          }
          Serial.println("[CMD:STATUS_RADAR]");
    }


    else if (texto.indexOf("desligar") != -1 && texto.indexOf("computador") != -1){
      bot.sendMessage(chatID, "Procurando o computador na rede...", "");

      // Fase 1: "voce esta aqui?" em binario
      udp.beginPacket("255.255.255.255", 5000);
      udp.print("01110110 01101111 01100011 01100101 00100000 01100101 01110011 01110100 01100001 00100000 01100001 01110001 01110101 01101001 00111111 00001010");
      udp.endPacket();

      char mac_str[18];
      snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", MAC_ALVO[0], MAC_ALVO[1], MAC_ALVO[2], MAC_ALVO[3], MAC_ALVO[4], MAC_ALVO[5]);
      String mac_esperado = String(mac_str);

      unsigned long tempo_inicio = millis();
      bool pc_encontrado = false;
    
      while (millis() - tempo_inicio < 2000){
        int tamanho_pacote = udp.parsePacket();
        if (tamanho_pacote){
          char resposta_buffer[255];
          int tamanho_lido = udp.read(resposta_buffer, 255);
          resposta_buffer[tamanho_lido] = '\0';
          String resposta = String(resposta_buffer);
          resposta.toUpperCase();

          if (resposta.indexOf("01100101 01110011 01110100 01101111 01110101 00100000 01100001 01110001 01110101 01101001 00001010") != -1 
          && resposta.indexOf(mac_esperado) != -1){
            ip_pc = udp.remoteIP();
            pc_encontrado = true;
            break;
          }
        }
      }

      // Fase 2: O Protocolo de Insistência
      if (pc_encontrado) {
        bot.sendMessage(chatID, "PC encontrado! Iniciando protocolo de desligamento...", "");
        
        int tentativas = 0;
        bool pc_ainda_ligado = true;

        // Tenta atirar até 5 vezes (aguardando 10s entre os tiros)
        while (pc_ainda_ligado && tentativas < 5) {
            tentativas++;
            String msg_tentativa = "Tentativa de desligamento " + String(tentativas) + "/5";
            bot.sendMessage(chatID, msg_tentativa, "");
            
            // 1. Envia o comando fatal
            udp.beginPacket(ip_pc, 5000);
            udp.print("01000100 01000101 01010011 01001100 01001001 01000111 01000001 01010010 01011111 01010000 01000011");
            udp.endPacket();

            // 2. Aguarda 10 segundos (dividido em pequenos delays para não bugar o watchdog do ESP)
            for(int d = 0; d < 10; d++) {
                delay(1000);
            }

            // 3. Manda um novo Ping para testar se o PC já "morreu"
            udp.beginPacket("255.255.255.255", 5000);
            udp.print("01110110 01101111 01100011 01100101 00100000 01100101 01110011 01110100 01100001 00100000 01100001 01110001 01110101 01101001 00111111 00001010");
            udp.endPacket();

            unsigned long tempo_checagem = millis();
            pc_ainda_ligado = false; // Presume que desligou, a não ser que receba resposta

            // 4. Ouve por 2 segundos. Se o PC responder, a variável volta para true e o loop repete
            while (millis() - tempo_checagem < 2000) {
                int tamanho = udp.parsePacket();
                if (tamanho) {
                    char resp_buf[255];
                    int t_lido = udp.read(resp_buf, 255);
                    resp_buf[t_lido] = '\0';
                    String resp = String(resp_buf);
                    resp.toUpperCase();

                    if (resp.indexOf("01100101 01110011 01110100 01101111 01110101 00100000 01100001 01110001 01110101 01101001 00001010") != -1 
                    && resp.indexOf(mac_esperado) != -1) {
                        pc_ainda_ligado = true; // Opa, o PC ainda está vivo!
                        break; 
                    }
                }
            }
        }

        // Relatório Final para o Telegram
        if (pc_ainda_ligado) {
            bot.sendMessage(chatID, "Alerta: Após 5 tentativas, o PC ainda está respondendo na rede.", "");
        } else {
            bot.sendMessage(chatID, "Sucesso: O computador parou de responder e foi desligado.", "");
        }

      } else {
        bot.sendMessage(chatID, "Falha: Computador não respondeu ao ping inicial.", "");
      }
    }
    else if (texto.indexOf("ligar") != -1 && texto.indexOf("computador") != -1){
      bot.sendMessage(chatID, "Comando recebido!\nLigando o computador", "");

      liga_pc_wol();

      delay(2000); 

      // 3. Envia a segunda mensagem de confirmação
      bot.sendMessage(chatID, "\nComputador ligado!", "");
    }
  }
}

void tarefaRadar(void *pvParameters){
  for(;;){
    csiLoop();
    vTaskDelay(1/portTICK_PERIOD_MS); //atraso de 1ms obrigatorio para evitar que o watchdog timer
                                    //da esp reinicie por achar que a placa travou
  }
}

void setup() {
  pinMode(pino_led, OUTPUT);
  digitalWrite(pino_led, LOW);

  Serial.begin(921600);
  while (!Serial) {
    delay(10);
  }

  Serial.println("Iniciando o sistema (bot com radar)");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wifi");

  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print("estabilizando...");
  }

  Serial.println("");
  Serial.println("Wi-fi conectado com sucesso");
  Serial.println("Endereço IP: " + WiFi.localIP().toString());

  client.setInsecure();
  ultimo_momento_visto = millis();

  csiSetup();
  // tarefa isolada para o nucleo 0
  xTaskCreatePinnedToCore(
    tarefaRadar,
    "TaskRadar",
    10000,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  if (millis() - tuv > intervalo_varredura) {
    tuv = millis();
    
    if (ip_pc[0] != 0) {
      if (tcpClient.connect(ip_pc, 5000)) {
        digitalWrite(pino_led, HIGH); 
        tcpClient.stop();
      } else {
        digitalWrite(pino_led, LOW);
        ip_pc = IPAddress(0, 0, 0, 0);
      }
    } else { 
      digitalWrite(pino_led, LOW);
      
      udp.beginPacket("255.255.255.255", 5000);
      udp.print("01110110 01101111 01100011 01100101 00100000 01100101 01110011 01110100 01100001 00100000 01100001 01110001 01110101 01101001 00111111 00001010");
      udp.endPacket();

      // Monta o MAC esperado para conferir a resposta
      char mac_str[18];
      snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", MAC_ALVO[0], MAC_ALVO[1], MAC_ALVO[2], MAC_ALVO[3], MAC_ALVO[4], MAC_ALVO[5]);
      String mac_esperado = String(mac_str);

      unsigned long tempo_escuta = millis();
      while (millis() - tempo_escuta < 1000) {
        if (udp.parsePacket()) {
          char res_buf[255];
          int len = udp.read(res_buf, 255);
          res_buf[len] = '\0';
          String res = String(res_buf);
          res.toUpperCase();

          // Se a resposta bater com a senha e o seu MAC
          if (res.indexOf("01100101 01110011 01110100 01101111 01110101 00100000 01100001 01110001 01110101 01101001 00001010") != -1 
              && res.indexOf(mac_esperado) != -1) {
            
            ip_pc = udp.remoteIP(); // MÁGICA: Capturou o IP do PC!
            Serial.print("PC Encontrado automaticamente! IP: ");
            Serial.println(ip_pc);
            break; // Sai do while
          }
        }
      }
    }
  }

  // ===================================================
  // ROTINA DO TELEGRAM (Roda sem bloqueios)
  // ===================================================
  int conta_nova_mensagem = bot.getUpdates(bot.last_message_received + 1);
  
  while (conta_nova_mensagem) {
    gerencia_mensagem(conta_nova_mensagem);
    conta_nova_mensagem = bot.getUpdates(bot.last_message_received + 1);    
  }
}