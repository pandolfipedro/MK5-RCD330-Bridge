# MK5 RCD330 Bridge

Ponte CAN para **Jetta MK5 2009 com RCD330**, usando **um ESP32 WROOM, um transceptor para o TWAI interno e um módulo MCP2515 com transceptor**. O objetivo principal é apresentar informações reais de rádio/mídia na página de áudio do painel vermelho, traduzindo BAP para DDP.

**Estado: protótipo de firmware.** A compilação para ESP32 e os testes de lógica em computador passaram. A disponibilidade dos textos na RCD330 e o funcionamento no carro ainda precisam de validação física. O código não foi testado nesse veículo.

O gateway do carro já está atualizado e o volume no volante já funciona. O firmware encaminha os comandos existentes sem remapeá-los.

## Estrutura

~~~text
MK5-RCD330-Bridge/
├── MK5_RCD330_Bridge/
│   └── MK5_RCD330_Bridge.ino
├── tests/
│   ├── protocol-check.js
│   └── ota-check.js
├── .gitattributes
├── .gitignore
└── README.md
~~~

O firmware é um único [.ino](MK5_RCD330_Bridge/MK5_RCD330_Bridge.ino). As bibliotecas utilizadas acompanham o pacote Arduino-ESP32; não é necessário instalar uma biblioteca externa para o MCP2515.

## O que está implementado

- Encaminhamento CAN nos dois sentidos: carro ↔ rádio.
- TWAI no lado do carro e MCP2515 no lado da RCD330.
- Remontagem BAP com verificação de sequência, tamanho e timeout.
- Extração de frequência, estação, fonte, título e artista no formato tratado pelo parser.
- Cliente BAP com solicitações **GET**, tentativas limitadas e rota configurável.
- Sessão DDP para registrar a página **Audio** e enviar duas linhas de texto, com rolagem.
- Controle da janela negociada, confirmações, busy e disponibilidade da área do painel.
- Wi-Fi com ponto de acesso oculto, permanentemente ligado.
- Página local para diagnóstico, configuração, captura CAN em CSV e atualização OTA.
- Configurações persistentes em NVS; títulos de áudio mantidos apenas em RAM.

A RCD330 continua no endereço de diagnóstico **56 — Rádio**. O firmware não altera coding/adaptações e não acessa o módulo 5F.

## Limite principal: obter os textos da RCD330

Ter um menu ou uma transferência DDP confirmada não comprova que a rádio forneceu informações de áudio.

A interface separa o texto recebido, a identificação da fonte e as confirmações DDP. Textos extraídos pelo fallback são identificados como interpretação a confirmar. Não são inseridos títulos de teste automaticamente.

As rotas BAP candidatas são **0x6C9, 0x6C8 e 0x67C**. A busca é limitada e ocorre somente no segmento da rádio. Elas são hipóteses de comunicação configuráveis, não uma sequência homologada para todos os firmwares RCD330. Respostas correlacionadas ajudam a escolher a rota; a confirmação depende de observar os dados mudando junto com a rádio.

Se a unidade não disponibilizar esses metadados na CAN, a ponte não consegue reconstruí-los apenas interceptando os fios.

## Hardware e ligações

~~~text
CAN de infotainment do carro
          ↕
 Transceptor do TWAI
          ↕ D4 / D5
      ESP32 WROOM
          ↕ SPI
 MCP2515 + transceptor
          ↕
       CAN da RCD330
~~~

Os dois segmentos CAN são separados. A instalação prevista usa uma **extensão Quadlock**, interceptando somente o par CAN da rádio e preservando o chicote original. Os demais fios continuam passando normalmente pela extensão.

### Pinos do ESP32

| Sinal | Pino |
|---|---|
| TWAI TX, configuração padrão | D4 / GPIO4 |
| TWAI RX, configuração padrão | D5 / GPIO5 |
| MCP2515 SCK | D18 / GPIO18 |
| MCP2515 SO / MISO | D19 / GPIO19 |
| MCP2515 SI / MOSI | D23 / GPIO23 |
| MCP2515 CS | D13 / GPIO13 |
| MCP2515 INT | D14 / GPIO14 |
| Referência comum | GND |

**D4/D5 permanecem nos pinos já soldados.** Se a ligação existente for TX em D5 e RX em D4, selecione essa opção na página de configuração. Confira a associação TXD/RXD antes de conectar ao barramento. A opção muda o firmware e não exige trocar os fios.

D2 e D12 não são usados. O SPI é configurado explicitamente com CS em D13, evitando ocupar D5.

### Alimentação e barramento

- TWAI: o controlador está no ESP32; o módulo externo precisa conter um transceptor adequado.
- MCP2515: precisa do controlador **e** do transceptor CAN. O nome MCP2515 sozinho não define a alimentação da placa inteira.
- Os GPIOs do ESP32 trabalham em **3,3 V e não toleram 5 V**. Uma placa MCP2515/TJA1050 pode exigir adaptação dos níveis SPI/INT.
- O step-down deve fornecer a tensão apropriada à entrada da placa de desenvolvimento; a previsão é **5 V em VIN**, após conferir a placa. Não aplicar os 12 V do carro diretamente ao ESP32.
- Usar GND comum.
- Configuração CAN: **100 kbit/s nos dois segmentos**.
- Cristal MCP2515: **8 MHz por padrão**, com opção de 16 MHz na interface. Não há autodetecção.
- Conferir camada física e terminação em cada segmento. Não adicionar ou retirar resistores de 120 Ω indiscriminadamente.
- Usar o par CAN de infotainment correto do chicote da rádio. Este README não estabelece a numeração dos terminais do seu adaptador Quadlock.

## Primeira gravação por USB

1. Abra o arquivo da pasta **MK5_RCD330_Bridge** no Arduino IDE.
2. Instale o pacote **esp32 por Espressif Systems, versão 3.3.12**, usado na verificação da compilação.
3. Selecione **ESP32 Dev Module**, para ESP32 clássico/WROOM.
4. Selecione flash de **4 MB**, se corresponder à sua placa, e **Default 4MB with spiffs**, com suporte a OTA.
5. Confira as constantes de Wi-Fi, as senhas e a associação D4/D5 antes de gravar.
6. Grave pela porta USB correspondente à placa.
7. Para acompanhar a inicialização, use o monitor serial a **115200 baud**.

O firmware precisa de partições OTA desde essa primeira gravação. Um particionamento sem OTA exige nova gravação por USB.

## Acesso ao Wi-Fi

Adicione a rede manualmente no telefone ou computador, pois o SSID é oculto.

| Item | Valor padrão |
|---|---|
| SSID | MK5-RCD330 |
| Segurança da rede | WPA2 |
| Senha Wi-Fi | Mk5Radio!330 |
| Página local | http://192.168.4.1 |
| Usuário web | admin |
| Senha web | PainelMK5!330 |

As credenciais são constantes no início do .ino; podem ser alteradas antes da gravação.

O ponto de acesso permanece ligado mesmo sem atividade CAN. **Não há deep-sleep.** O consumo acrescentado pelo ESP32, módulos e step-down precisa ser medido antes de deixar o conjunto alimentado permanentemente.

## Atualizações futuras pela página web

1. Edite o .ino e compile usando a mesma placa e um particionamento com OTA.
2. No Arduino IDE, use **Sketch → Export Compiled Binary**.
3. Conecte-se à rede oculta e abra a página local.
4. Em **Atualizar o ESP32**, selecione o arquivo **.ino.bin da aplicação**.
5. Confirme a pausa da comunicação CAN e envie o arquivo.
6. Aguarde a confirmação. O ESP32 reinicia ao concluir.
7. Reconecte-se à rede e confira a versão no diagnóstico.

Não envie arquivos **merged.bin, bootloader.bin ou partitions.bin**. O upload verifica o cabeçalho, o modelo ESP32 clássico e o descritor de aplicação; a biblioteca de atualização valida a imagem ao concluir.

A ponte é pausada durante o upload, interrompendo temporariamente a comunicação CAN da rádio. Em caso de falha no envio, o código retoma a ponte. Essa atualização modifica **somente o ESP32**, não o firmware da RCD330.

Não há dependência de internet, CDN ou servidor externo. Os .bin são arquivos temporários de atualização e estão ignorados pelo Git; nenhum binário faz parte deste repositório.

## Configuração e diagnóstico

Na página local é possível:

- Selecionar a associação TX/RX de D4/D5.
- Selecionar cristal MCP2515 de 8 ou 16 MHz.
- Usar a busca de rota BAP ou fixar uma das rotas candidatas.
- Ajustar a função BAP de texto, inicialmente **0x15**.
- Ativar/desativar as solicitações BAP e a tradução para o painel.
- Reiniciar a negociação e baixar a captura CAN recente.

Salvar configurações reinicia o ESP32. O campo **Transferências DDP confirmadas** conta confirmações de transporte; não é uma medição visual do que apareceu no painel.

### Validação no carro

1. Confira se há recepção dos dois lados e se as funções atuais da rádio/volume permanecem funcionando.
2. Mude a estação/frequência e observe os textos recebidos na página web.
3. Mude a fonte e a música e confira se os campos acompanham essas mudanças.
4. Com áudio válido recebido, aguarde a negociação DDP. O início tem um atraso de aproximadamente **20 segundos após a atividade do carro**.
5. Confira os mesmos dados na página Audio do painel vermelho.
6. Navegue para consumo/autonomia e verifique se o áudio não força o retorno da página.
7. Teste desligamento e nova partida.

O critério de sucesso é **informação real da RCD330 atualizando no painel**, não somente registro de uma página ou recebimento de ACK.

### Quando faltarem dados

| Sintoma | Conferir |
|---|---|
| Wi-Fi não aparece na lista | Adicionar o SSID oculto manualmente |
| TWAI iniciado, mas RX do carro em zero | Associação D4/D5, barramento e camada física |
| MCP2515 não inicia | SPI, CS/INT, alimentação/níveis e cristal 8/16 MHz |
| Há CAN, mas não há texto BAP | Captura recente, rota, função e disponibilidade dos dados na rádio |
| Texto aparece na web, mas não no painel | Sessão DDP, disponibilidade da área e seleção da página Audio |
| OTA informa ausência de partição | Refazer a gravação USB com particionamento OTA |
| OTA rejeita o arquivo | Usar a aplicação para ESP32 clássico, não imagem combinada ou de outro chip |

## Verificações em computador

A versão do firmware foi compilada com Arduino-ESP32 **3.3.12**, placa **esp32:esp32:esp32**, flash 4 MB e partição **default**:

- Programa: **982.360 bytes**, de 1.310.720 disponíveis.
- Variáveis globais: **69.792 bytes**, de 327.680.

Os testes extraem as rotinas do próprio .ino e as exercitam em computador, com simulação do hardware:

- Codificação e leitura de quadros MCP2515 padrão, estendidos e RTR.
- Limites de fila; remontagem BAP, canais intercalados, sequência, timeout e tamanho inválido.
- Campos de áudio e normalização de caracteres.
- Negociação DDP, ACK incorreto, janela de quatro quadros, busy, seleção de área e timeout.
- Upload OTA: autenticação, token, fragmentos, arquivo único, cabeçalho, modelo do chip, tamanho, abortamento e falhas de gravação.
- Sintaxe do JavaScript embutido e configuração do ponto de acesso oculto.

**Esses testes não substituem um teste de bancada, o upload OTA físico ou a validação no carro.**

### Executar os testes

Requer Node.js e um compilador C++17. As verificações anteriores usaram **Zig 0.15.2** no Windows. Execute na raiz do repositório, com node e zig disponíveis no PATH:

~~~powershell
node tests/protocol-check.js
node tests/ota-check.js
zig c++ -std=c++17 -O0 -g -o .test-build/protocol_test.exe .test-build/protocol_test.cpp
.\.test-build\protocol_test.exe
zig c++ -std=c++17 -O0 -g -o .test-build/ota_test.exe .test-build/ota_test.cpp
.\.test-build\ota_test.exe
~~~

Os arquivos gerados ficam em **.test-build/**, ignorada pelo Git. Os scripts JavaScript geram os testes C++; é necessário compilar e executar os dois programas para verificar as asserções de protocolo e OTA.

## Referências

- [MIB2-Can-Bridge — projeto-base de tradução](https://github.com/tehfalcon-vw/MIB2-Can-Bridge).
- [revag-docs — notas de BAP e fragmentação](https://github.com/norly/revag-docs/blob/master/notes/bap.txt).
- [Golf-Mk5-RCD330-CAN-Filter — filtro CAN para MK5/RCD330](https://github.com/unemployable/Golf-Mk5-RCD330-CAN-Filter).
- [PCCar — relato de comunicação com a tela do painel VW](https://pccar.ru/showthread.php?nojs=1&t=24102).
- [Microchip — MCP2515](https://www.microchip.com/en-us/product/mcp2515).

As referências documentam partes da comunicação; não representam uma homologação deste firmware na sua RCD330.

## Git local

O repositório foi inicializado na branch **main**, sem remoto configurado. O commit inicial reúne o firmware, este README e os testes.

Para registrar alterações futuras, execute na raiz do projeto:

~~~powershell
git status
git diff
git add README.md MK5_RCD330_Bridge tests
git commit -m "Descreva a alteração"
~~~

O arquivo .gitignore mantém firmware compilado, objetos e saídas de testes fora dos commits.
