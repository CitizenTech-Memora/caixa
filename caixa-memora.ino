// Integrantes (RM):
// Eric Kang (RM 572575)
// Guilherme Tome Nogueira (RM 570144)
// Lucas de Andrade Astorini (RM 569119)
// Sabrina Lopes da Silva (RM 571870)
// Sofia Satomi Hagio (RM 569120)

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <DHT.h>
#include <ESP32Servo.h>

//PINOS
#define PINO_DHT      4
#define PINO_BOTAO    13   // botao ABRIR
#define PINO_DEMO     16   // botao DEMO 
#define PINO_BUZZER   23
#define LED_VERDE     25
#define LED_AMARELO   26
#define LED_VERMELHO  27

// cada compartimento tem um servo, um LDR e um LED (manha, tarde, noite)
int pinoServo[3]   = {18, 17, 14};
int pinoLDR[3]     = {34, 35, 36};
int pinoLedComp[3] = {32, 33, 19};   // verde, amarelo, azul

//CONFIGURACOES
String nomeDose[3] = {"Manha", "Tarde", "Noite"};
int horaDose[3]    = {8, 14, 20};
int minutoDose[3]  = {0, 0, 0};

const bool MODO_DEMO = true;     // true = 1 segundo vale 1 minuto

const int TEMPO_LEMBRETE = 10;   // minutos
const int TEMPO_ATRASO   = 20;
const int TEMPO_LIMITE   = 60;

const float LIMITE_TEMP = 30.0;  
const int LUZ_CHEIO = 2500;      // leitura alta = escuro = remedio no lugar
const int LUZ_VAZIO = 1000;      // leitura baixa = claro = remedio retirado

const String PACIENTE = "Dona Maria (paciente ficticia)";
const bool MQTT_LIGADO = false;          
const bool WHATSAPP_CONFIGURADO = false; 

//ESTADOS
enum Estado { AGUARDANDO, HORA_DA_DOSE, LEMBRETE, DOSE_ATRASADA,
              DOSE_RETIRADA, FORA_DE_HORA, ARMAZENAMENTO_RUIM };
String nomeEstado[7] = {"AGUARDANDO", "HORA_DA_DOSE", "LEMBRETE", "DOSE_ATRASADA",
                        "DOSE_RETIRADA", "FORA_DE_HORA", "ARMAZENAMENTO_RUIM"};

Estado estado = AGUARDANDO;
Estado estadoAntesDoCalor = AGUARDANDO;
unsigned long inicioEstado = 0;

//COMPONENTES
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
DHT dht(PINO_DHT, DHT22);
Servo servo[3];

//VARIAVEIS
int doseAtual = -1;
bool doseFeitaHoje[3] = {false, false, false};
int diaAtual = -1;
bool estavaCheio = false;
int ultimoAviso = 0;

int ultimaDoseRetirada = -1;
uint32_t horaRetirada = 0;
String horaRetiradaTexto = "";

// media movel das ultimas 5 leituras
int leiturasLDR[3][5];
int indiceLDR = 0;
float leiturasTemp[5];
int indiceTemp = 0;
int qtdTemp = 0;
float temperatura = 0;

bool compartimentoVazio[3] = {false, false, false};
int contVazio[3] = {0, 0, 0};
int contCheio[3] = {0, 0, 0};

bool erroSensor = false;
int falhasSimuladas = 0;

// relogio 
uint32_t baseRelogio = 0;
unsigned long baseMillis = 0;

unsigned long ultimaLeituraLDR = 0;
unsigned long ultimaLeituraDHT = 0;
unsigned long ultimaTela = 0;

bool botaoAntes = HIGH;
bool demoAntes = HIGH;
unsigned long ultimoClique = 0;

String linha1Antes = "";
String linha2Antes = "";

//SONS 
const int SOM_LEMBRETE[] = {523, 659, 784, 0, 784, 659, 523};
const int SOM_ALERTA[]   = {880, 0, 880, 0, 880, 0, 880};
const int SOM_BIPE[]     = {1000};
const int SOM_OK[]       = {784, 1047};
const int SOM_CALOR[]    = {1200, 0, 1200};

int notas[10];
int totalNotas = 0;
int notaAtual = 0;
unsigned long duracaoNota = 0;
unsigned long inicioNota = 0;
bool tocando = false;

void tocarNota() {
  if (notas[notaAtual] == 0) noTone(PINO_BUZZER);
  else tone(PINO_BUZZER, notas[notaAtual]);
  inicioNota = millis();
}

// comeca a tocar um som, as notas vao trocando sozinhas no atualizarBuzzer()
void tocarSom(const int som[], int quantidade, int duracao) {
  for (int i = 0; i < quantidade; i++) notas[i] = som[i];
  totalNotas = quantidade;
  duracaoNota = duracao;
  notaAtual = 0;
  tocando = true;
  tocarNota();
}

void atualizarBuzzer() {
  if (!tocando) return;
  if (millis() - inicioNota >= duracaoNota) {
    notaAtual++;
    if (notaAtual >= totalNotas) {
      noTone(PINO_BUZZER);
      tocando = false;
    } else {
      tocarNota();
    }
  }
}

//RELOGIO 
// pega a hora atual do RTC
DateTime agora() {
  if (!MODO_DEMO) return rtc.now();
  uint32_t segundos = (millis() - baseMillis) / 1000;
  return DateTime(baseRelogio + segundos * 60);
}

void ajustarRelogio(DateTime novo) {
  rtc.adjust(novo);
  baseRelogio = novo.unixtime();
  baseMillis = millis();
}

String doisDigitos(int n) {
  if (n < 10) return "0" + String(n);
  return String(n);
}

String horaTexto() {
  DateTime a = agora();
  return doisDigitos(a.hour()) + ":" + doisDigitos(a.minute());
}

String horaDaDose(int i) {
  return doisDigitos(horaDose[i]) + ":" + doisDigitos(minutoDose[i]);
}

int minutosDoDia() {
  DateTime a = agora();
  return a.hour() * 60 + a.minute();
}

// quantos minutos ja passaram desde o horario da dose atual
int minutosDesdeDose() {
  return minutosDoDia() - (horaDose[doseAtual] * 60 + minutoDose[doseAtual]);
}

int proximaDose() {
  int m = minutosDoDia();
  for (int i = 0; i < 3; i++) {
    if (horaDose[i] * 60 + minutoDose[i] > m) return i;
  }
  return 0;  // ja passou a da noite: a proxima e a da manha
}

// quando vira o dia, as 3 doses ficam liberadas de novo
void verificarNovoDia() {
  int dia = agora().day();
  if (dia != diaAtual) {
    diaAtual = dia;
    for (int i = 0; i < 3; i++) doseFeitaHoje[i] = false;
  }
}

//EVENTOS / MQTT / WHATSAPP 
void enviarMQTT(String topico, String mensagem) {
  if (!MQTT_LIGADO) return;
}

// mostra o evento no Serial Monitor (hora, tipo, estado anterior -> novo e descricao)
void registrarEvento(String tipo, Estado de, Estado para, String descricao) {
  Serial.println();
  Serial.println("[" + horaTexto() + "] " + tipo);
  if (de != para) Serial.println("  Estado: " + nomeEstado[de] + " -> " + nomeEstado[para]);
  else Serial.println("  Estado: " + nomeEstado[de]);
  Serial.println("  " + descricao);
  enviarMQTT("memora/eventos", horaTexto() + ";" + tipo + ";" + nomeEstado[de] + ";" + nomeEstado[para] + ";" + descricao);
}

// monta a mensagem de WhatsApp para o cuidador
void avisarCuidador(String status, String descricao) {
  String mensagem = " ALERTA MEMORA\n";
  mensagem += "Atenção, cuidador!\n";
  mensagem += "Paciente: " + PACIENTE + "\n";
  mensagem += descricao + "\n";
  mensagem += "Horário: " + horaTexto() + "\n";
  mensagem += "Status: " + status + "\n";
  mensagem += "Verifique se está tudo bem com o paciente.";

  Serial.println();
  Serial.println("------ WHATSAPP DO CUIDADOR ------");
  Serial.println(mensagem);
  if (WHATSAPP_CONFIGURADO) Serial.println(" Enviado ao back-end para o WhatsApp do cuidador.");
  else Serial.println(" Mensagem enviada para o cuidador.");


  enviarMQTT("memora/notificacoes", PACIENTE + ";" + status + ";" + horaTexto() + ";" + descricao);
}

//SERVOS 
void travarTudo() {
  for (int i = 0; i < 3; i++) servo[i].write(0);
}

void abrirCompartimento(int i) {
  travarTudo();
  servo[i].write(90);
}

//SENSORES
// le os 3 LDRs e usa a media das ultimas 5 leituras para saber se esta cheio ou vazio
void lerLDRs() {
  for (int k = 0; k < 3; k++) {
    leiturasLDR[k][indiceLDR] = analogRead(pinoLDR[k]);

    int soma = 0;
    for (int i = 0; i < 5; i++) soma += leiturasLDR[k][i];
    int media = soma / 5;

    // precisa ficar 8 leituras seguidas (1,6 s) para mudar
    if (media < LUZ_VAZIO) {
      contVazio[k]++;
      contCheio[k] = 0;
      if (contVazio[k] >= 8) compartimentoVazio[k] = true;
    } else if (media > LUZ_CHEIO) {
      contCheio[k]++;
      contVazio[k] = 0;
      if (contCheio[k] >= 8) compartimentoVazio[k] = false;
    } else {
      contVazio[k] = 0;
      contCheio[k] = 0;
    }
  }
  indiceLDR = (indiceLDR + 1) % 5;
}

// le o DHT22 e calcula a media das ultimas 5 leituras validas
void lerTemperatura() {
  float t = dht.readTemperature();

  if (falhasSimuladas > 0) {   // comando 'f'
    t = NAN;
    falhasSimuladas--;
  }

  // leitura invalida e ignorada e nao muda o estado
  if (isnan(t)) {
    erroSensor = true;
    registrarEvento("ERRO_SENSOR", estado, estado, "DHT22 deu leitura invalida (NaN). Leitura ignorada.");
    return;
  }

  if (erroSensor) {
    erroSensor = false;
    registrarEvento("SENSOR_OK", estado, estado, "DHT22 voltou a funcionar.");
  }

  leiturasTemp[indiceTemp] = t;
  indiceTemp = (indiceTemp + 1) % 5;
  if (qtdTemp < 5) qtdTemp++;

  float soma = 0;
  for (int i = 0; i < qtdTemp; i++) soma += leiturasTemp[i];
  temperatura = soma / qtdTemp;
}

// LEDS 
void atualizarLEDs() {
  bool piscaLento = (millis() / 500) % 2;
  bool piscaRapido = (millis() / 150) % 2;

  int verde = 0;
  bool amarelo = false;
  bool vermelho = false;

  switch (estado) {
    case AGUARDANDO:
    case HORA_DA_DOSE:
    case LEMBRETE:
      verde = 25;  // aceso fraquinho
      break;
    case DOSE_ATRASADA:
      vermelho = true;
      break;
    case DOSE_RETIRADA:
      verde = 255;
      break;
    case FORA_DE_HORA:
      vermelho = piscaRapido;
      break;
    case ARMAZENAMENTO_RUIM:
      amarelo = true;
      break;
  }

  analogWrite(LED_VERDE, verde);
  digitalWrite(LED_AMARELO, amarelo);
  digitalWrite(LED_VERMELHO, vermelho);

  // LED do compartimento: aceso na hora da dose e piscando no lembrete/atraso
  for (int i = 0; i < 3; i++) {
    bool ligado = false;
    if (i == doseAtual) {
      if (estado == HORA_DA_DOSE) ligado = true;
      if (estado == LEMBRETE) ligado = piscaLento;
      if (estado == DOSE_ATRASADA) ligado = piscaRapido;
    }
    digitalWrite(pinoLedComp[i], ligado);
  }
}

//LCD
String completar(String texto) {
  while (texto.length() < 16) texto += " ";
  return texto.substring(0, 16);
}

void escreverLCD(String l1, String l2) {
  l1 = completar(l1);
  l2 = completar(l2);
  if (l1 != linha1Antes) { lcd.setCursor(0, 0); lcd.print(l1); linha1Antes = l1; }
  if (l2 != linha2Antes) { lcd.setCursor(0, 1); lcd.print(l2); linha2Antes = l2; }
}

String textoTemperatura() {
  if (erroSensor || qtdTemp == 0) return "T:ERRO";
  return "T:" + String(temperatura, 1) + "C";
}

// mostra no LCD a mensagem de cada estado
void atualizarTela() {
  int p = proximaDose();
  switch (estado) {
    case AGUARDANDO:
      escreverLCD(horaTexto() + " " + textoTemperatura(), "Prox:" + horaDaDose(p) + " " + nomeDose[p]);
      break;
    case HORA_DA_DOSE:
      escreverLCD("HORA DO REMEDIO!", "Retire: " + nomeDose[doseAtual]);
      break;
    case LEMBRETE:
      escreverLCD("Retire o remedio", nomeDose[doseAtual] + "  +" + String(minutosDesdeDose()) + "min");
      break;
    case DOSE_ATRASADA:
      escreverLCD("DOSE ATRASADA!", nomeDose[doseAtual] + "  +" + String(minutosDesdeDose()) + "min");
      break;
    case DOSE_RETIRADA:
      escreverLCD("Remedio retirado", "da caixa " + horaRetiradaTexto);
      break;
    case FORA_DE_HORA:
      escreverLCD("Fora do horario!", "Libera as " + horaDaDose(p));
      break;
    case ARMAZENAMENTO_RUIM:
      escreverLCD("ALERTA TEMPERAT.", textoTemperatura() + " Max:30C");
      break;
  }
}

//MAQUINA DE ESTADOS
// troca de estado, registra o evento e faz a acao de entrada do novo estado
void mudarEstado(Estado novo, String tipo, String descricao) {
  Estado anterior = estado;
  estado = novo;
  inicioEstado = millis();
  registrarEvento(tipo, anterior, novo, descricao);

  // o que acontece quando entra em cada estado
  switch (novo) {
    case AGUARDANDO:
      travarTudo();
      break;
    case HORA_DA_DOSE:
      abrirCompartimento(doseAtual);
      tocarSom(SOM_LEMBRETE, 7, 180);
      break;
    case LEMBRETE:
      ultimoAviso = minutosDesdeDose();
      tocarSom(SOM_LEMBRETE, 7, 180);
      break;
    case DOSE_ATRASADA:
      ultimoAviso = minutosDesdeDose();
      tocarSom(SOM_ALERTA, 7, 250);
      break;
    case DOSE_RETIRADA:
      travarTudo();
      tocarSom(SOM_OK, 2, 150);
      break;
    case FORA_DE_HORA:
      travarTudo();
      tocarSom(SOM_BIPE, 1, 150);
      break;
    case ARMAZENAMENTO_RUIM:
      tocarSom(SOM_CALOR, 3, 120);
      break;
  }
  atualizarTela();
}

bool ehEstadoDeDose(Estado e) {
  return e == HORA_DA_DOSE || e == LEMBRETE || e == DOSE_ATRASADA;
}

bool temDoseLiberada() {
  if (ehEstadoDeDose(estado)) return true;
  return estado == ARMAZENAMENTO_RUIM && ehEstadoDeDose(estadoAntesDoCalor);
}

void iniciarDose(int i) {
  doseAtual = i;
  doseFeitaHoje[i] = true;
  estavaCheio = !compartimentoVazio[i];
  mudarEstado(HORA_DA_DOSE, "DOSE_LIBERADA",
              "Dose da " + nomeDose[i] + " (" + horaDaDose(i) + ") liberada. So esse compartimento abriu.");
}

// so conta como retirada se o compartimento estava cheio e depois ficou vazio
bool remedioRetirado() {
  if (!compartimentoVazio[doseAtual]) {
    estavaCheio = true;
    return false;
  }
  return estavaCheio;
}

void registrarRetirada() {
  ultimaDoseRetirada = doseAtual;
  horaRetirada = agora().unixtime();
  horaRetiradaTexto = horaTexto();
  mudarEstado(DOSE_RETIRADA, "MEDICAMENTO_RETIRADO",
              "Remedio da " + nomeDose[doseAtual] + " retirado da caixa as " + horaRetiradaTexto +
              " (+" + String(minutosDesdeDose()) + " min). Nao da para saber se foi tomado.");
}

void encerrarDose(String motivo) {
  String texto = "Dose da " + nomeDose[doseAtual] + " NAO foi retirada. " + motivo;
  doseAtual = -1;
  if (estado == ARMAZENAMENTO_RUIM) {
    estadoAntesDoCalor = AGUARDANDO;
    travarTudo();
    registrarEvento("DOSE_NAO_RETIRADA", estado, estado, texto);
  } else {
    mudarEstado(AGUARDANDO, "DOSE_NAO_RETIRADA", texto);
  }
}

// estado 7: entra quando passa de 30 C e volta ao estado anterior quando esfria
void verificarTemperatura() {
  if (qtdTemp == 0) return;

  if (estado != ARMAZENAMENTO_RUIM && temperatura > LIMITE_TEMP) {
    if (estado == FORA_DE_HORA) estadoAntesDoCalor = AGUARDANDO;
    else estadoAntesDoCalor = estado;
    mudarEstado(ARMAZENAMENTO_RUIM, "TEMPERATURA_ALTA",
                "Temperatura media de " + String(temperatura, 1) + " C, acima do limite.");
    avisarCuidador("TEMPERATURA ALTA",
                   "A temperatura da caixa está em " + String(temperatura, 1) + " °C, acima do limite de 30 °C.");
  }
  else if (estado == ARMAZENAMENTO_RUIM && temperatura < LIMITE_TEMP - 1) {
    mudarEstado(estadoAntesDoCalor, "TEMPERATURA_NORMAL",
                "Temperatura voltou para " + String(temperatura, 1) + " C.");
  }
}

// aqui o sistema decide o proximo estado (switch/case com os 7 estados)
void maquinaDeEstados() {
  verificarTemperatura();

  switch (estado) {
    case AGUARDANDO:
      for (int i = 0; i < 3; i++) {
        int m = minutosDoDia() - (horaDose[i] * 60 + minutoDose[i]);
        if (m >= 0 && m < TEMPO_LIMITE && !doseFeitaHoje[i]) {
          iniciarDose(i);
          break;
        }
      }
      break;

    case HORA_DA_DOSE:
      if (remedioRetirado()) registrarRetirada();
      else if (minutosDesdeDose() >= TEMPO_LEMBRETE)
        mudarEstado(LEMBRETE, "LEMBRETE", "Dose da " + nomeDose[doseAtual] + " nao retirada em 10 min.");
      break;

    case LEMBRETE:
      if (remedioRetirado()) registrarRetirada();
      else if (minutosDesdeDose() >= TEMPO_ATRASO) {
        mudarEstado(DOSE_ATRASADA, "DOSE_ATRASADA", "Dose da " + nomeDose[doseAtual] + " nao retirada em 20 min.");
        avisarCuidador("DOSE ATRASADA", "A dose da " + nomeDose[doseAtual] + " não foi retirada no horário previsto.");
      }
      else if (minutosDesdeDose() - ultimoAviso >= 5) {  // repete o som a cada 5 min
        ultimoAviso = minutosDesdeDose();
        tocarSom(SOM_LEMBRETE, 7, 180);
      }
      break;

    case DOSE_ATRASADA:
      if (remedioRetirado()) registrarRetirada();
      else if (minutosDesdeDose() >= TEMPO_LIMITE) encerrarDose("Passou 1 hora, compartimento travado.");
      else if (minutosDesdeDose() - ultimoAviso >= 10) {  // repete o alarme a cada 10 min
        ultimoAviso = minutosDesdeDose();
        tocarSom(SOM_ALERTA, 7, 250);
      }
      break;

    case DOSE_RETIRADA:
      if (millis() - inicioEstado >= 5000) {
        doseAtual = -1;
        mudarEstado(AGUARDANDO, "AGUARDANDO", "Tudo travado. Esperando a proxima dose.");
      }
      break;

    case FORA_DE_HORA:
      if (millis() - inicioEstado >= 3000)
        mudarEstado(AGUARDANDO, "AGUARDANDO", "Fim do aviso de fora de horario.");
      break;

    case ARMAZENAMENTO_RUIM:
      // a saida desse estado fica no verificarTemperatura()
      if (temDoseLiberada() && minutosDesdeDose() >= TEMPO_LIMITE)
        encerrarDose("Passou 1 hora durante o alerta de temperatura.");
      break;
  }
}

//BOTOES
// detecta o momento em que o botao foi apertado (ignora repeticoes por 200 ms)
bool apertou(int pino, bool &antes) {
  bool leitura = digitalRead(pino);
  bool clicou = (antes == HIGH && leitura == LOW && millis() - ultimoClique > 200);
  if (clicou) ultimoClique = millis();
  antes = leitura;
  return clicou;
}

// botao ABRIR apertado fora do horario: continua travado e avisa o cuidador
void tentativaForaDeHora() {
  bool duplicada = ultimaDoseRetirada >= 0 && agora().unixtime() - horaRetirada < 3600;
  if (duplicada) {
    mudarEstado(FORA_DE_HORA, "TENTATIVA_FORA_DE_HORA",
                "A dose da " + nomeDose[ultimaDoseRetirada] + " ja foi retirada. Possivel dose duplicada.");
    avisarCuidador("TENTATIVA FORA DE HORA", "O paciente tentou abrir a caixa de novo, mas a dose da " +
                   nomeDose[ultimaDoseRetirada] + " já foi retirada (possível dose duplicada).");
  } else {
    mudarEstado(FORA_DE_HORA, "TENTATIVA_FORA_DE_HORA",
                "Tentou abrir fora do horario. Proxima dose: " + horaDaDose(proximaDose()) + ".");
    avisarCuidador("TENTATIVA FORA DE HORA",
                   "O paciente tentou abrir a caixa fora do horário. O compartimento continuou travado.");
  }
}

void pularParaDose(int i, bool amanha) {
  if (temDoseLiberada()) encerrarDose("Horario adiantado na demonstracao.");
  doseAtual = -1;

  DateTime a = agora();
  DateTime dose(a.year(), a.month(), a.day(), horaDose[i], minutoDose[i], 0);
  uint32_t alvo = dose.unixtime() - 5 * 60;
  if (amanha) alvo += 24 * 60 * 60;
  ajustarRelogio(DateTime(alvo));
  verificarNovoDia();
  doseFeitaHoje[i] = false;

  String texto = "[DEMO] Relogio ajustado para " + horaTexto() + ".";
  if (estado == ARMAZENAMENTO_RUIM) {
    estadoAntesDoCalor = AGUARDANDO;
    registrarEvento("AJUSTE_HORARIO", estado, estado, texto);
  } else if (estado != AGUARDANDO) {
    mudarEstado(AGUARDANDO, "AJUSTE_HORARIO", texto);
  } else {
    registrarEvento("AJUSTE_HORARIO", estado, estado, texto);
  }
}

void lerBotoes() {
  if (apertou(PINO_BOTAO, botaoAntes)) {
    if (temDoseLiberada()) tocarSom(SOM_LEMBRETE, 7, 180);   // repete o lembrete
    else if (estado == ARMAZENAMENTO_RUIM) tocarSom(SOM_BIPE, 1, 150);
    else tentativaForaDeHora();
  }

  if (apertou(PINO_DEMO, demoAntes)) {
    int m = minutosDoDia();
    int proxima = -1;
    for (int i = 0; i < 3; i++) {
      if (horaDose[i] * 60 + minutoDose[i] - 5 > m) { proxima = i; break; }
    }
    if (proxima >= 0) pularParaDose(proxima, false);
    else pularParaDose(0, true);
  }
}

//SERIAL
void mostrarStatus() {
  Serial.println();
  Serial.println("===== STATUS =====");
  Serial.println("Hora: " + horaTexto() + " | Estado: " + nomeEstado[estado]);
  Serial.println("Temperatura media: " + textoTemperatura());
  for (int i = 0; i < 3; i++) {
    String situacao = compartimentoVazio[i] ? "VAZIO" : "CHEIO";
    Serial.println(nomeDose[i] + ": " + situacao + " | LDR = " + String(analogRead(pinoLDR[i])));
  }

}

void lerSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'm') pularParaDose(0, false);
    if (c == 't') pularParaDose(1, false);
    if (c == 'n') pularParaDose(2, false);
    if (c == 's') mostrarStatus();
    if (c == 'f') {
      falhasSimuladas = 3;
      Serial.println("[TESTE] As proximas 3 leituras do DHT22 vao falhar.");
    }
  }
}

//SETUP E LOOP
void setup() {
  Serial.begin(115200);

  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_DEMO, INPUT_PULLUP);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  for (int i = 0; i < 3; i++) {
    pinMode(pinoLedComp[i], OUTPUT);
    pinMode(pinoLDR[i], INPUT);
  }

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  escreverLCD("     MEMORA", "  iniciando...");

  for (int i = 0; i < 3; i++) {
    servo[i].setPeriodHertz(50);
    servo[i].attach(pinoServo[i], 500, 2400);
  }
  travarTudo();

  dht.begin();

  if (!rtc.begin()) Serial.println("Erro: RTC nao encontrado!");
  if (!rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  DateTime inicio = rtc.now();
  if (MODO_DEMO) inicio = DateTime(inicio.year(), inicio.month(), inicio.day(), 7, 55, 0);
  ajustarRelogio(inicio);
  verificarNovoDia();

  // enche a media do LDR com a primeira leitura
  for (int k = 0; k < 3; k++) {
    int primeira = analogRead(pinoLDR[k]);
    for (int i = 0; i < 5; i++) leiturasLDR[k][i] = primeira;
  }

  Serial.println();
  Serial.println("===== MEMORA - porta-remedios inteligente =====");
  Serial.println("Modo demo: 1 s = 1 min | comandos: m, t, n, f, s");
  registrarEvento("SISTEMA_INICIADO", AGUARDANDO, AGUARDANDO,
                  "Memora ligado. Proxima dose: " + horaDaDose(proximaDose()) + ".");
}

void loop() {
  lerSerial();
  lerBotoes();
  verificarNovoDia();

  if (millis() - ultimaLeituraLDR >= 200) {
    ultimaLeituraLDR = millis();
    lerLDRs();
  }
  if (millis() - ultimaLeituraDHT >= 2000) {
    ultimaLeituraDHT = millis();
    lerTemperatura();
  }

  maquinaDeEstados();

  if (millis() - ultimaTela >= 300) {
    ultimaTela = millis();
    atualizarTela();
  }

  atualizarLEDs();
  atualizarBuzzer();
}
