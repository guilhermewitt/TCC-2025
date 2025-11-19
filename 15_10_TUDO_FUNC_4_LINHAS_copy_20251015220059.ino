#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <HX711.h>

// --- CONFIGURAÇÕES DO DISPLAY ---
LiquidCrystal_I2C lcd(0x27, 20, 4);

// --- PINAGEM ---
// Botões
const int pinoBotaoModoManual = 5;
const int pinoBotaoModoAuto = 6;
const int pinoBotaoReset = 7;
const int pinoBotaoIniciar = 8;
const int pinoAumentaRPM = 11; // Usa INPUT_PULLUP
const int pinoDiminuiRPM = 12; // Usa INPUT_PULLUP
// LEDs de status
const int pinoLedVerde = A0;
const int pinoLedVermelho = A1;
// Célula de Carga
const int LOADCELL_DOUT_PIN = 9;
const int LOADCELL_SCK_PIN = 10;
// Sensor DHT
#define DHTPIN 13
#define DHTTYPE DHT11
// Motor de Passo
const int ENABLE_PIN = 2;
const int DIR_PIN = 4;
const int PUL_PIN = 3;

// --- CONSTANTES DE OPERAÇÃO ---
const int RPM_AUTOMATICO_FIXO = 300;
const int RPM_MINIMO = 10;
const int RPM_MAXIMO = 300;
const int RPM_INCREMENTO = 10;
const int PASSOS_POR_REVOLUCAO = 1600;

// Constantes calculadas (sem "números mágicos")
const unsigned long FATOR_CONVERSAO_RPM_US = 60000000UL / PASSOS_POR_REVOLUCAO;
const float FATOR_CALIBRACAO_HX711 = 2280.0;
const float FATOR_CORRECAO_TRACAO = 1.2;

// --- MÁQUINA DE ESTADOS ---
enum EstadoSistema {
  SELECIONANDO_MODO,
  AJUSTE_MANUAL,
  PRONTO_PARA_INICIAR,
  OPERANDO
};
EstadoSistema estadoAtual;

// --- VARIÁVEIS GLOBAIS ---
float umidadeAtual = 0.0, tracaoAtual = 0.0;
int modoOperacao = 0; // 1 para Manual, 2 para Automático
int rpmDesejadoManual = 60;
int rpmAtualOperando = 0;
unsigned long contadorDePassos = 0;
unsigned long ultimoTempoDisplay = 0, ultimoTempoUmidade = 0;
const unsigned long INTERVALO_DISPLAY_MS = 500;
const unsigned long INTERVALO_UMIDADE_MS = 2000;
unsigned long ultimoTempoPasso_us = 0;
unsigned long intervaloMeioPasso_us = 0;
bool estadoPinoPulso = LOW;
bool estadosAnterioresBotoes[14] = {false};

// --- OBJETOS ---
DHT dht(DHTPIN, DHTTYPE);
HX711 scale;

// --- FUNÇÕES DE LÓGICA E CONTROLE ---
void atualizarStatusLEDs(bool motorEstaLigado) {
  digitalWrite(pinoLedVerde, motorEstaLigado);
  digitalWrite(pinoLedVermelho, !motorEstaLigado);
}

bool lerBotao(int pino, bool activeLow = false) {
  bool estadoPressionado = activeLow ? LOW : HIGH;
  bool estadoAtual = digitalRead(pino);
  bool foiPressionado = false;

  if (estadoAtual != estadosAnterioresBotoes[pino]) {
    delay(20);
    estadoAtual = digitalRead(pino);
    if (estadoAtual == estadoPressionado && estadosAnterioresBotoes[pino] != estadoPressionado) {
      foiPressionado = true;
    }
  }
  estadosAnterioresBotoes[pino] = estadoAtual;
  return foiPressionado;
}

// --- FUNÇÕES DO DISPLAY ---
void exibirTelaInicial() {
  lcd.clear();
  lcd.setCursor(1, 0); lcd.print("SELECIONE O MODO");
  lcd.setCursor(0, 2); lcd.print("1-Auto   2-Manual");
}

void exibirTelaAjusteManual() {
  lcd.clear();
  lcd.setCursor(4, 0); lcd.print("MODO MANUAL");
  lcd.setCursor(2, 2); lcd.print("Ajuste RPM: "); lcd.print(rpmDesejadoManual);
}

void exibirTelaProntoAuto() {
  lcd.clear();
  lcd.setCursor(2, 0); lcd.print("MODO AUTOMATICO");
  lcd.setCursor(0, 2); lcd.print("Pressione INICIAR");
}

void atualizarDisplayOperacao() {
  float voltas = (float)contadorDePassos / PASSOS_POR_REVOLUCAO;
  char buffer[21];

  lcd.setCursor(0, 0);
  if (isnan(umidadeAtual)) {
    lcd.print("Umidade: Erro DHT   ");
  } else {
    char umidadeStr[6];
    dtostrf(umidadeAtual, 4, 1, umidadeStr);
    sprintf(buffer, "Umidade: %s%%      ", umidadeStr);
    lcd.print(buffer);
  }
  
  char tracaoStr[8];
  dtostrf(tracaoAtual, 5, 2, tracaoStr);
  sprintf(buffer, "Tracao:  %s", tracaoStr);
  lcd.setCursor(0, 1);
  lcd.print(buffer);

  sprintf(buffer, "RPM:     %-4d", rpmAtualOperando);
  lcd.setCursor(0, 2);
  lcd.print(buffer);

  char voltasStr[8];
  dtostrf(voltas, 6, 1, voltasStr);
  sprintf(buffer, "Voltas:  %s", voltasStr);
  lcd.setCursor(0, 3);
  lcd.print(buffer);
}

// --- FUNÇÕES DE ESTADO ---
void pararOperacao() {
  digitalWrite(ENABLE_PIN, LOW);
  digitalWrite(PUL_PIN, LOW);
  estadoPinoPulso = LOW;
  atualizarStatusLEDs(false);

  if (modoOperacao == 1) {
    estadoAtual = AJUSTE_MANUAL;
    exibirTelaAjusteManual();
  } else {
    estadoAtual = PRONTO_PARA_INICIAR;
    exibirTelaProntoAuto();
  }
}

void resetarSistema() {
  digitalWrite(ENABLE_PIN, LOW);
  digitalWrite(PUL_PIN, LOW);
  estadoPinoPulso = LOW;
  contadorDePassos = 0;
  modoOperacao = 0;
  estadoAtual = SELECIONANDO_MODO;
  atualizarStatusLEDs(false);
  exibirTelaInicial();
}

void iniciarOperacao() {
  if (modoOperacao == 1) {
    rpmAtualOperando = rpmDesejadoManual;
  } else if (modoOperacao == 2) {
    rpmAtualOperando = RPM_AUTOMATICO_FIXO;
  }
  
  intervaloMeioPasso_us = (FATOR_CONVERSAO_RPM_US / rpmAtualOperando) / 2;
  
  contadorDePassos = 0;
  ultimoTempoPasso_us = micros();
  estadoAtual = OPERANDO;
  atualizarStatusLEDs(true);
  lcd.clear();
}

// --- SETUP ---
void setup() {
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();
  dht.begin();
  
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(FATOR_CALIBRACAO_HX711);
  scale.tare();

  pinMode(pinoBotaoModoManual, INPUT);
  pinMode(pinoBotaoModoAuto, INPUT);
  pinMode(pinoBotaoReset, INPUT);
  pinMode(pinoBotaoIniciar, INPUT);
  pinMode(pinoAumentaRPM, INPUT_PULLUP);
  pinMode(pinoDiminuiRPM, INPUT_PULLUP);
  
  pinMode(ENABLE_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(PUL_PIN, OUTPUT);
  pinMode(pinoLedVerde, OUTPUT);
  pinMode(pinoLedVermelho, OUTPUT);
  
  digitalWrite(DIR_PIN, HIGH);

  resetarSistema();
}

// --- LOOP PRINCIPAL ---
void loop() {
  if (lerBotao(pinoBotaoReset)) {
    resetarSistema();
  }

  switch (estadoAtual) {
    
    case SELECIONANDO_MODO:
      if (lerBotao(pinoBotaoModoManual)) {
        modoOperacao = 1;
        estadoAtual = AJUSTE_MANUAL;
        exibirTelaAjusteManual();
      }
      if (lerBotao(pinoBotaoModoAuto)) {
        modoOperacao = 2;
        estadoAtual = PRONTO_PARA_INICIAR;
        exibirTelaProntoAuto();
      }
      break;

    case AJUSTE_MANUAL:
      if (lerBotao(pinoAumentaRPM, true)) {
        rpmDesejadoManual = min(RPM_MAXIMO, rpmDesejadoManual + RPM_INCREMENTO);
        exibirTelaAjusteManual();
      }
      if (lerBotao(pinoDiminuiRPM, true)) {
        rpmDesejadoManual = max(RPM_MINIMO, rpmDesejadoManual - RPM_INCREMENTO);
        exibirTelaAjusteManual();
      }
      if (lerBotao(pinoBotaoIniciar)) {
        iniciarOperacao();
      }
      break;

    case PRONTO_PARA_INICIAR:
      if (lerBotao(pinoBotaoIniciar)) {
        iniciarOperacao();
      }
      break;

    case OPERANDO:
      // A lógica agora depende do modo de operação selecionado
      if (modoOperacao == 1) { // MODO MANUAL: Funciona apenas enquanto o botão estiver pressionado
        
        if (digitalRead(pinoBotaoIniciar) == HIGH) {
          // Se o botão ESTÁ pressionado, execute a lógica do motor
          digitalWrite(ENABLE_PIN, HIGH);
          unsigned long tempoAtual_us = micros();
          if (tempoAtual_us - ultimoTempoPasso_us >= intervaloMeioPasso_us) {
            ultimoTempoPasso_us = tempoAtual_us;
            estadoPinoPulso = !estadoPinoPulso;
            digitalWrite(PUL_PIN, estadoPinoPulso);
            
            if (estadoPinoPulso == LOW) {
              contadorDePassos++;
            }
          }
        } else {
          // Se o botão foi SOLTO, pare a operação
          pararOperacao();
          break; // Sai do case para efetivar a parada
        }

      } else { // MODO AUTOMÁTICO: Funciona com um pulso para ligar/desligar (toggle)
        
        // Verifica se o botão foi clicado para PARAR
        if (lerBotao(pinoBotaoIniciar)) {
          pararOperacao();
          break; // Sai do case
        }

        // Se não foi clicado para parar, continua executando a lógica do motor
        digitalWrite(ENABLE_PIN, HIGH);
        unsigned long tempoAtual_us = micros();
        if (tempoAtual_us - ultimoTempoPasso_us >= intervaloMeioPasso_us) {
          ultimoTempoPasso_us = tempoAtual_us;
          estadoPinoPulso = !estadoPinoPulso;
          digitalWrite(PUL_PIN, estadoPinoPulso);
          
          if (estadoPinoPulso == LOW) {
            contadorDePassos++;
          }
        }
      }

      // --- TAREFAS COMUNS A AMBOS OS MODOS (Sensores e Display) ---
      if (scale.is_ready()) {
        tracaoAtual = scale.get_units() * FATOR_CORRECAO_TRACAO;
      }
      
      unsigned long tempoAtual_ms = millis();
      if (tempoAtual_ms - ultimoTempoUmidade >= INTERVALO_UMIDADE_MS) {
        ultimoTempoUmidade = tempoAtual_ms;
        umidadeAtual = dht.readHumidity();
      }
      
      if (tempoAtual_ms - ultimoTempoDisplay >= INTERVALO_DISPLAY_MS) {
        ultimoTempoDisplay = tempoAtual_ms;
        atualizarDisplayOperacao();
      }
      break;
  }
}