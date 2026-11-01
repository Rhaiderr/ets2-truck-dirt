# Compatibilidade: as suposições do plugin sobre o render do jogo

O desenho da sujeira depende de alguns fatos do render do ETS2/ATS. Uma atualização do jogo ou um mod (de
caminhão ou gráfico) pode mudar qualquer um deles. Desde a N11 o plugin confere esses fatos sozinho, com o
que já observa a cada quadro (`src/compat.*`, ligado em `src/tint.cpp`). Quando um fato deixa de valer:

- desliga **só a parte afetada** do desenho (o desenho todo ou só a câmera interna);
- a simulação, as camadas e o `dirt_state.txt` continuam funcionando;
- registra o motivo no `gpu_probe.log` e no log do jogo, com o número medido e o limite. Nada aparece na tela.

Com `[compat] enforce = 0` no INI, nada é desligado: as linhas saem com "would turn ... off (enforce = 0:
only logged)". Serve para testar.

No fim da sessão sai uma linha-resumo: `compatibility: all ok`, ou o que foi desligado e por quê
(`turned off: ...`), mais os avisos (`warnings: ...`). A cada 10 s o `StatsLine` traz `compat: ...`, com o
estado e a fração medida de draws sem matriz.

Todas as linhas começam com `compat <segundos> s: <checagem>:`. Os limites estão no relatório da N11 e são
todos [estimativa].

## Tabela

| # | Suposição | Como foi verificada | Se mudar | Linha do log que denuncia | O que acontece |
|---|---|---|---|---|---|
| 1 | O jogo só escreve os bits `0x0f` do stencil; o plugin usa `0x70`. Desde a N21 os 3 bits são 7 códigos: cavalo `0x10`, cabine `0x50`, `trailer.0` `0x20`, `trailer.1`–`4` `0x30`, `0x40`, `0x60`, `0x70`. | [verificado: V3a, o G-buffer escreve `0x0f`, `ref 1`; ATS 1.61 com rodotrem, D4u, 28/10] | A poeira cairia em pixels de outra coisa, ou o plugin apagaria bits que o jogo usa depois. | `stencil: game state #N (ops ...) writes bits 0x.. of ours (0x70): mask 0x.., ref 0x..` | Desenho desligado na sessão. Não religa: o estado continua existindo. |
| 2a | O G-buffer tem 4+ alvos: rt0–2 `R16G16B16A16_FLOAT`, rt3 `R16G16B16A16_UINT`, profundidade com stencil (`D32_S8X24` ou `D24_S8`). | [verificado: V3a, 4 alvos sem SnowyMoon, 7 com ele; ATS 1.61 com o SnowyMoon, 7 alvos, D4p–D4s, 28/10] | O plugin não acha o pass e não desenha nada. É seguro, mas a sujeira some. | `g-buffer: a pass with 4+ targets is not taken as the G-buffer: DXGI formats rt0-3 a, b, c, d, depth e` (uma vez por conjunto de formatos, até 8) | Só log. No resumo só entra se nenhum G-buffer apareceu na sessão. |
| 2b | Com o jogo rodando (sem pausa) e o caminhão configurado, existe um pass de G-buffer a cada quadro. | [verificado: V3a, 28 capturas] | Igual ao 2a. | `g-buffer: no G-buffer pass for 15.0 s with the game running and the truck in it` / `G-buffer pass seen again` | Só log. |
| 3 | A VS cb0 dos draws do G-buffer traz a matriz modelo→câmera nas linhas 0–2 (e a projeção nas linhas 4, 5 e 7). Exceção (N29): shaders que põem constantes próprias antes dela, com a matriz nas linhas 1–3 seguida de `0 0 0 1`; o plugin aceita esse caso, sem tirar a projeção dele. | [verificado: `testdata/rig`, 114 de 32601 draws sem matriz = 0,35 %; ATS 1.61, a pintura do Western Star 57X com a matriz uma linha abaixo, captura 2 da D4r, 28/10] | O rastreio pegaria peças erradas e a poeira cairia fora do lugar. | `matrix: X% of the G-buffer draws have no model->camera matrix in VS cb0 (a of b in the last 5 s, 2 windows in a row; limit 25%, normal ~0.4%)` | Desenho desligado na sessão. Não religa. |
| 4 | As rodas e as peças de cada unidade são achadas pela geometria (rodas da telemetria, caixa da unidade). | [verificado: V3a e teste no jogo da D3c] | Um modelo de mod incomum fica sem poeira. Seguro: nada é pintado errado. | `parts: truck|trailer in the game, G-buffer drawn, none of its parts recognised for 60.0 s` / `recognised again` | Só log. |
| 5 | A chuva é vista na GPU: texturas `rain.tobj` e `blobs_rain*.tobj` pelos hashes, ou as gotas no vidro (`def.glassdrops`). | [verificado: V2b, também com o Weather 3.9] | A chuva passa a vir só do limpador (telemetria): lava menos quando o limpador está desligado. | `rain: wipers on for 120.0 s and no rain seen on the GPU` / `rain seen on the GPU again` | Só log. |
| 6 | Na câmera interna, a cabine é desenhada com stencil `ref 2`, e o rastreio a ignora. | [verificado: V3a, ~86 draws `ref 2` por quadro na vista principal das 15 capturas internas] | Painel e bancos entrariam como peças do caminhão e ganhariam poeira. | `interior: no ref-2 draw (the cabin seen from inside) in 30.0 s of interior camera` / `ref-2 draws in the interior camera again` | Poeira desligada nos quadros da câmera interna. Religa quando o `ref 2` volta. |
| 6a | A câmera interna é o quadro com 2+ vistas de G-buffer: os espelhos só são desenhados nela. | [verificado: `testdata/rig`, 15 capturas internas com 4–5 vistas de espelho, 13 externas com uma vista] | Sem espelhos (desligados nas opções gráficas), a checagem 6 não roda. Se o jogo passar a desenhar espelhos na câmera externa, a 6 pode desligar a poeira nela à toa. | `StatsLine`: `compat: ... interior would be off` ou `INTERIOR OFF` com a câmera externa | Ver a 6. |
| 7 | (N30) O vidro do caminhão é desenhado depois da luz, multiplicado sobre a imagem (blend `DEST_COLOR`/`ZERO`), com teste de profundidade e sem gravar profundidade, no buffer de profundidade da câmera principal e na origem do caminhão. | [verificado: ETS2 V3a e ATS D4s; no ATS 32695 draws do vidro em 29/10] | O interior visto pela janela voltaria a pegar poeira (o vidro não seria reconhecido), ou um vidro de outro veículo esconderia a poeira atrás dele. | `cab glass (N30): N draws into its depth, M dust passes used it` no `StatsLine` (N = 0 com o caminhão à vista = o vidro não foi reconhecido) | Só log. A poeira continua; o interior pela janela pode sujar. |

Fora da autochecagem (já tratados em outro lugar):
- shaders compilados com o `d3dcompiler_47.dll` e objetos do passe criados (`tint off: ...`);
- rt0 legível por shader (`dust: rt0 cannot be read by a shader ...`);
- falha de acesso ao ler a cb0 (`tint: access fault reading VS cb0`);
- exceção nos hooks (`tint off: exception in the tint path`);
- outro plugin de D3D11 instalando hooks nas mesmas funções ao mesmo tempo (D6): `hook <nome> changed while hooking`,
  `off: ... kept changing while hooking`, `hook install lock not taken` (ver `the development notes`).

## Roteiro depois de uma atualização do ETS2/ATS (ou de um mod gráfico)
1. Rodar o jogo com o INI normal (`enforce = 1`). Dirigir 5 min na câmera externa e 2 min na interna, com
   espelhos. Pegar chuva, se der.
2. No `Documentos\<jogo>\TruckDirt\gpu_probe.log`:
   - procurar `compat ` (as linhas de decisão) e a linha-resumo no fim (`compatibility: ...`);
   - no último `StatsLine`, conferir `compat: drawing ok, no matrix 0.xx%` e o custo (`cost per frame:`).
3. `compatibility: all ok` e poeira no lugar: nada a fazer.
4. Se algo foi desligado: repetir com `enforce = 0`, olhar a poeira no jogo e decidir.
   - A poeira está certa: a suposição mudou sem estragar o desenho. Ajustar o limite em `compat.h`
     (`Limits`) e registrar aqui.
   - A poeira está errada: a checagem fez o seu papel. Abrir uma tarefa para adaptar o plugin ao render
     novo, com uma captura V3a (`capture_s`).
5. Atualizar esta tabela: a coluna "Como foi verificada" ganha a versão do jogo conferida.

## Versões conferidas
- ETS2 1.61.1.9 e ATS 1.61.3.18, 28–29/10/2026 [verificado: o padrão do despachante da L7 nos dois executáveis e os
  testes D4k–D4v de `the development notes`]. No ATS, 13 mods ativos (com o SnowyMoon, 7 alvos), nenhum de pintura
  da carroceria. O SnowyMoon e o Weather 3.9 no ETS2 foram conferidos nas etapas V2b e D3.

## ATS (D7): o que muda em relação ao ETS2
O mesmo `ets2_truck_dirt.dll` roda nos dois jogos; o `game_id` do SDK diz qual (`g.ats` em `plugin.cpp`).

| Item | ATS | Situação |
|---|---|---|
| Pasta do plugin | `American Truck Simulator\bin\win_x64\plugins`, mesmo nome de DLL e de INI | [verificado: D4k–D4v instaladas assim] |
| Logs, INI de reserva e sujeira salva | `Documentos\American Truck Simulator\TruckDirt\` (`DirtDir()`), separados dos do ETS2 | [verificado: `plugin.cpp` e as sessões do ATS de 28–29/10] |
| Multiplayer | `core_atsmp.dll` desliga a GPU, como o `core_ets2mp.dll` | [verificado: `plugin.cpp`]; não testado no Convoy do ATS |
| Moeda da lavagem | o preço do INI está na moeda base do jogo: US$ no ATS (120 / 180), € no ETS2; a cobrança vai na moeda base, a dica mostra a moeda escolhida no menu (`display_rate`). O texto usa `_price` de `lang/en_us.txt` (`{currency}{amount}`): a ligação (sessão da lavagem) precisa passar `$` no ATS e `€` no ETS2 | ⏳ falta ligar e testar (L7) |
| Pintura com a matriz deslocada no cb0 | Western Star 57X (N29) | ✅ 28/10 |
| Rodas com o centro acima do raio do pneu | 57X: centro 0,511 m × raio 0,5065 m; disco até 1,04 × `wheel.radius` (N22) | 🟡 instalada, falta o teste dedicado |
| Rodotrem, dolly (até 5 carretas) | graneleira + dolly + graneleira (N21, N24) | ✅ 28/10 ("tudo parece certo agora") |
| SnowyMoon (7 alvos) | os shaders do ATS com o SnowyMoon não estão no catálogo; o plugin acha os passes pela forma | ✅ 28/10 |
| Chuva | as texturas de chuva são reconhecidas pelos hashes do ETS2 (`base.scs`); o ATS pode ter outros arquivos | ⏳ não verificado: dirigir na chuva no ATS e procurar `rain` no `gpu_probe.log` (sem nada, só o limpador lava) |
| Vidro do 57X (N30) | 5 vidros na origem do caminhão | ✅ 29/10 |
| Outros caminhões do ATS (VNL, bicudos antigos) | medida das rodas (N20) só no VNL e na graneleira | ⏳ amostra pequena |
