# Plano da 1.10.0 — RTX 30 em campo, admissão por módulo, matriz de compatibilidade, base e idiomas

Status: **aprovado em 2026-11-10; em execução.** Branch `feat/1.10.0` a partir de `feat/1.9.0` (`1226d4f`).

Decisão de 2026-11-10: **tudo numa release só**, com seis idiomas no painel. Versão: 1.10.0 (muda comportamento em pré-Ada, acrescenta idiomas e matriz; não é 2.0 porque nada muda para RTX 40/50 nem na instalação).

## Decisões de 2026-11-10

1. HAGS do relator: desconhecido. O addon passa a registrar.
2. Lista do Calledme_kev (Opti Discord, RTX 3060 Ti, SL 2.14.16, 1.9.0): XMX funciona em 11 jogos (Titans of the Tide, Robocop x2, Expedition 33, Fort Solis, Hell is Us, HellBlade 2, Metal Eden, Valor Mortis, Samson, Ghost of Tsushima). Não funciona em Rift Apart, Keeper e The Outer Worlds 2. RDR Remastered parcial (multiplicador errado até reiniciar).
3. Rift Apart: a opção de FG no menu era ligável.
4. OW2 e Keeper: os logs são da 1.8.6; um relato na 1.9.0 ainda falta.
5. Driver 617.42 é real.
6. Escopo de API: D3D12 por enquanto.
7. Uma release só: diagnóstico, correção, matriz, base e idiomas. Os relatores recebem um **build de teste** antes da release, como na 1.9.0.
8. Idiomas do painel: inglês (base), português do Brasil, espanhol neutro, japonês, coreano e chinês simplificado (sem tradicional). Não há revisor na comunidade: os cinco idiomas saem marcados "tradução automática" no seletor, e correções entram por PR.
9. Versão 1.10.0; branch `feat/1.10.0`.

Convenção: **Verificado** = provado por código, log, binário ou reprodução nesta máquina. **Hipótese** = sem prova, em ordem de probabilidade.

## O que a investigação já provou

1. **OW2 e Keeper (1.8.6): o plugin aceitou e nunca criou a feature.** `slInit entered … admission scope set`, nenhuma linha `Streamline DLSS-G admission: 6 -> ok` (o suporte nativo respondeu OK), `wrapped slDLSSGSetOptions`, `overriding … accepted it`, `confirms NVIDIA Dynamic MFG support`. Nunca aparece a linha de [ObserveCreate](../../src/addons/mfgunlock/preada_admission.hpp#L427), que loga sempre que [HookedCreateFeature](../../src/addons/mfgunlock/addon_provider.inl#L72) vê uma criação de FG, nem `xmx evaluate`. O hook estava instalado (`installed 1 hook(s) in _nvngx.dll`). Zero frames apresentados até o fim.
2. **Rift Apart (1.9.0): `slInit` já estava em execução quando o addon carregou.** [HookedInit](../../src/addons/mfgunlock/framecount.hpp#L1998) loga incondicionalmente e nunca logou; o cartão diz `Streamline start: not called since the addon loaded`; mesmo assim o plugin foi mapeado depois do hook (`stopped the DLSS-G plugin from lowering its compiled ceiling` em 07:42:26:972), o que só acontece dentro de `slInit`. O `slIsFeatureSupported` nativo respondeu 6, o addon respondeu OK ao jogo, mas o plugin manager seguiu com a máscara vazia: nenhum `wrapped slDLSSGSetOptions`, `Game request: Not observed`. A linha `slInit hook installed before Streamline loaded its DLSS-G plugin` ([framecount.hpp:3004](../../src/addons/mfgunlock/framecount.hpp#L3004)) é um falso positivo nesse caso.
3. **A lista do Calledme_kev prova que a cadeia funciona na mesma placa com o mesmo SL 2.14.16.** As três falhas são por jogo, não uma recusa universal do plugin.
4. **O plugin lê `FrameGeneration.Available` e tem modos inertes.** Strings do `sl.dlss_g.dll` 2.14.16 (build development em `build/sl-samples/sample-v2.14.16`): `NGX indicates DLSS-G is not available - DLSS-G cannot run`, `Disabling DLSS-G since it is not supported on current hardware`, `Missing NGX default parameters - DLSS-G cannot run`, `DLSS-G disabled: window not focused`, `Cannot obtain common constants`, `Failed to find global tag '%s'`. Em qualquer um deles `slDLSSGSetOptions` pode seguir devolvendo OK.
5. **O interposer de produção aceita log por ambiente e registro.** Strings do `sl.interposer.dll` 2.14.16: `ConfigureLogOverridesFromEnvironment`, `SL_LOG_LEVEL`, `SL_LOG_PATH`, `SL_LOG_NAME`, `SL_ENABLE_CONSOLE_LOGGING`, e `SOFTWARE\NVIDIA Corporation\Global\Streamline` com `LogLevel`/`LogPath`. O arquivo é `sl.log`. O guia público não documenta isso; o binário sim.
6. **A admissão é escopo por thread e por janela.** [HookedArchInfo](../../src/addons/mfgunlock/preada_admission.hpp#L91) só responde Ada com `g_in_admission`, ligado em [InitScope](../../src/addons/mfgunlock/preada_admission.hpp#L228) (dentro de `slInit`) e em [HookedIsFeatureSupported](../../src/addons/mfgunlock/preada_admission.hpp#L143). O startup do plugin (quando o device é setado) e um `slInit` em curso ficam fora.
7. **O addon só intercepta o slot 11 (`Get` de `unsigned int`) e só `DLSSG.MultiFrameCountMax`** ([PatchParameterVTable](../../src/addons/mfgunlock/addon.cpp#L579), [ngx_param_vtable.hpp:17](../../src/addons/mfgunlock/ngx_param_vtable.hpp#L17)). `FrameGeneration.Available` é lido como `int` (slot 12).
8. **Uma fábrica DXGI é criada a cada segundo no Present** ([addon_latency.inl:476](../../src/addons/mfgunlock/addon_latency.inl#L476), `UpdateVramDiagnostics`). É a origem de todas as linhas `Redirecting CreateDXGIFactory1(riid = {770AAE78…})` a 1 Hz nos logs.
9. **Painel e fonte.** Cerca de 1.670 literais nos arquivos do painel (`addon_overlay.inl` 1215, `native_controls.cpp` 295, `native_panel.cpp` 94, `panel_ui.hpp` 54, `native_ui.cpp` 12); descontando IDs `##` e linhas de log, perto de 900 visíveis ao usuário. O ReShade usa uma fonte para toda a interface; o addon define Cascadia Mono no primeiro start se não houver fonte escolhida ([addon.cpp:1353](../../src/addons/mfgunlock/addon.cpp#L1353)). Cascadia Mono não tem glifos chineses, japoneses nem coreanos.

## Hipóteses

**Rift Apart**
- R1 (provável): o jogo importa `sl.interposer.dll`, e o interposer carrega `dxgi.dll` por nome dentro de `slInit`; o `dxgi.dll` do ReShade e o addon entram aí. `LoadFromDllMain` nunca será cedo o bastante nesse jogo. A checagem de arquitetura do plugin roda fora do escopo.
- R2: o jogo faz checagem própria. Contra: o menu era ligável.

**OW2 e Keeper**
- A1: o plugin re-checa a GPU no startup (fora da janela). Contra: 11 jogos, vários UE5, funcionam na mesma placa com o mesmo plugin. Só vale se o startup desses dois tiver ordem diferente.
- A2: `FrameGeneration.Available=0` para esses dois (populate do NGX antes do mínimo rebaixado, ou cópia do driver escolhida). Contra: `Dynamic MFG supported` veio do NGX populando o snippet.
- A3: condição de runtime do plugin (`window not focused`, tag global ausente, constantes inválidas, formato). Os dois usam swapchain `R10G10B10A2` com flags `0x842`; os que funcionam, não se sabe.
- A4: HAGS desligado. Contra: 11 jogos funcionam no mesmo PC.

Só o `sl.log` separa A1 a A4. Numa release única, o build de teste com o bloco 1 vai aos relatores enquanto os blocos 2 a 5 são feitos; o bloco 2 fecha com o que o `sl.log` disser.

## Bloco 1: diagnóstico (sem mudar comportamento)

### 1.1 Log do Streamline pelo addon

**O que muda:** chave `[DLSSG] StreamlineLog=1` em `MFGAdaUnlock.ini` (e botão no painel, página Support). No `DLL_PROCESS_ATTACH`, o addon chama `SetEnvironmentVariableW` com `SL_LOG_LEVEL=2`, `SL_LOG_PATH=<pasta do jogo>`, `SL_LOG_NAME=sl.log`. Vale para jogos que chamam `slInit` depois do addon (UE5). Para o caso Rift Apart, o painel mostra o comando do registro (`HKCU\SOFTWARE\NVIDIA Corporation\Global\Streamline`, `LogLevel`=2, `LogPath`), sem escrever no registro sozinho.
**Arquivos:** `addon.cpp` (ATTACH), `addon_overlay.inl` (Support), `support_files.hpp` (incluir `sl.log` no "Save support files").
**Teste:** `xemfg_startup_tests.py` ganha um caso com a chave; teste manual no SL Sample 2.14.16 confirmando que `sl.log` aparece.
**Ganho esperado:** zero em FPS. Em cada relato, uma linha do próprio plugin dizendo por que não gera. Fecha A1 a A4 numa rodada.

### 1.2 Valores nativos do bloco de capacidade

**O que muda:** o hook do slot 11 passa a registrar, uma vez por nome, os valores nativos de `FrameGeneration.Available`, `FrameGeneration.NeedsUpdatedDriver`, `FrameGeneration.MinDriverVersionMajor/Minor`, `DLSSG.MultiFrameCountMax`; o slot 12 (`int`) ganha o mesmo hook só para registrar. Nada é alterado neste bloco.
**Arquivos:** `ngx_param_vtable.hpp` (segundo `VTablePatch`), `addon.cpp` (`HookedGetInt32`), teste `ngx_param_vtable_tests.cpp`.
**Ganho esperado:** prova ou descarta A2 sem o relator.

### 1.3 "Feature nunca criada" no painel e no log

**O que muda:** com `Admitted()` e `slDLSSGSetOptions` ligado há mais de 5 s sem `HookedCreateFeature` de FG, uma linha de aviso e uma linha "NGX feature: not created since the game turned frame generation on" no cartão da cadeia (vermelho). O "Generator" do XMX deixa de dizer só "not being called".
**Arquivos:** `addon_provider.inl` (`DescribeFrameGenChain`), `addon_overlay.inl`, `preada_admission.hpp` (timestamp do primeiro SetOptions ligado).
**Teste:** `fg_chain_tests.cpp`.

### 1.4 HAGS, driver e NGX core no início do log (o "doctor")

**O que muda:** uma linha `mfgunlock: system: HAGS on/off (HwSchMode=N), driver X, NGX core Y, Windows build Z` em pré-Ada, lida de `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\HwSchMode` e da versão de `_nvngx.dll`. No painel, um card na primeira execução: HAGS, driver, NGX core, versão do provider, do SL, early load, duplicatas, um veredito por linha, botão "Copy". HAGS desligado vira aviso amarelo (o guia do Streamline exige HWS).
**Arquivos:** `system_info.hpp`, `addon.cpp`, `addon_overlay.inl`.
**Teste:** `diagnostics_tests.cpp`.
**Ganho esperado:** cada relato chega com o mesmo cabeçalho; elimina as perguntas 1 e 5 deste plano.

### 1.5 Cartão da cadeia: "slInit em curso"

**O que muda:** quando o hook de `slInit` entrou, `HookedInit` não rodou e o plugin foi mapeado depois do hook, o cartão diz "started before the addon could hook it (the game loads ReShade from inside Streamline)". A linha `slInit hook installed before Streamline loaded its DLSS-G plugin` passa a dizer "no DLSS-G plugin mapped yet".
**Arquivos:** `addon_provider.inl`, `framecount.hpp`.

### 1.6 Hook de `CreateFeature` também no "DLSS-G ligado"

**O que muda:** [BeforeFrameGeneration](../../src/addons/mfgunlock/addon.cpp#L636) chama `TryInstallPresetHook()` depois de `PinResident`. Hoje jogos que ligam o FG antes do primeiro Present (Ghost of Tsushima) nunca marcam `g_create_seen`, e [ApplyTemporalCorrection](../../src/addons/mfgunlock/preada_admission.hpp#L354) usa esse flag para não reescrever um provider já em uso.
**Teste:** `dlssg_streamline_fixture` com SetOptions antes do Present.
**Ganho esperado:** fecha uma janela de risco real (reescrever PTX com a feature viva), não visível em FPS.

### 1.7 Fábrica DXGI cacheada e `get_color_space` fora do Present

**O que muda:** `UpdateVramDiagnostics` guarda fábrica e adaptador (recria se `IsCurrent()` falhar); `get_color_space` só na troca de swapchain e a 1 Hz.
**Ganho esperado:** FPS inalterado na prática (dezenas de microssegundos por segundo). O ganho é o log: cerca de metade das linhas dos três relatos somem.

### 1.8 `DEVICE_REMOVED` no DLSS-G

**O que muda:** `OnPresentStartDiscovery` observa `GetDeviceRemovedReason` da swapchain primária uma vez por segundo; ao falhar, loga a razão e desarma os hooks irreversíveis (XMX, preset) para a sessão.
**Ganho esperado:** o log explica o crash em vez de terminar em silêncio; sem efeito em jogo saudável.

**Build de teste 1** sai aqui, para os três relatores e para quem tiver um dos 11 jogos que funcionam (controle positivo).

## Bloco 2: correção em pré-Ada

### 2.1 Admissão por módulo chamador

**O que muda:** em pré-Ada, `EnsureArchHook` roda no load. [HookedArchInfo](../../src/addons/mfgunlock/preada_admission.hpp#L91) responde Ada quando `g_in_admission` **ou** quando `_ReturnAddress()` cai num módulo `sl.*.dll` ou no NGX core (`GetModuleHandleExW` com `FROM_ADDRESS`, cache por módulo). Complemento: no [loadhook](../../src/addons/mfgunlock/loadhook.hpp#L91), ao mapear `sl.common.dll` ou `sl.dlss_g.dll`, liga o escopo da thread corrente até o retorno do `LoadLibrary` que o mapeou.
**Arquivos:** `preada_admission.hpp`, `loadhook.hpp`, novo `tests/preada_caller_tests.cpp` (classificação de módulo chamador com DLLs falsas).
**Risco:** baixo. Hoje a janela de `slInit` já responde Ada para tudo naquela thread; o jogo continua recebendo a resposta real. Só sob `Admitted()`; RTX 40/50 intocadas.
**Ganho esperado:** jogos do tipo Rift Apart (interposer carrega o ReShade de dentro do `slInit`) passam a oferecer FG. Se A1 for a causa de OW2/Keeper, cobre os dois também. Se for A2/A3, não.

### 2.2 `FrameGeneration.Available` em pré-Ada

**O que muda:** o slot 12 responde 1 e `NeedsUpdatedDriver` 0, registrando o valor nativo. Numa release única, entra atrás de `[DLSSG] PreAdaCapability=1` e o padrão é decidido pelo `sl.log` do build de teste 1: ligado se algum relato mostrar `Available=0`, desligado se nenhum mostrar.
**Risco:** médio. Se o driver realmente não suportar a feature, o `CreateFeature` falha, o que já é observado e exibido.

### 2.3 Suporte do `slIsFeatureSupported` sem mentir

**O que muda:** com 2.1, [HookedIsFeatureSupported](../../src/addons/mfgunlock/preada_admission.hpp#L143) devolve OK só se o plugin estiver mapeado e a nova consulta no escopo aceitar; caso contrário devolve o nativo e o painel explica. Evita o estado do Rift Apart (menu ligável, nada acontece).

### 2.4 Hook de requisitos Vulkan (adiado)

Fora do escopo por decisão 6. Registrado: [TryInstallRequirements](../../src/addons/mfgunlock/preada_admission.hpp#L244) cobre só a exportação D3D12.

### 2.5 Overlays e OptiScaler no caminho DLSS-G

**O que muda:** a `native_factory_hook_failure` da 1.8.6 (módulo dono do hook e destino do primeiro jump) aplicada aos hooks de `slGetFeatureFunction` e `NVSDK_NGX_D3D12_EvaluateFeature`, com aviso nomeando o módulo (RTSS, Special K, OptiScaler). Esforço P.

**Build de teste 2** sai aqui.

## Bloco 3: matriz de compatibilidade

### 3.1 Oráculo sintético com o SL Sample (M)

`tests/dlssg_streamline_smoke.cpp` roda o SL Sample 2.14.16 e 2.11.1 (`build/sl-samples`) em: DLSS-G, XMX; `SimulatePreAda` ligado e desligado; ReShade como `dxgi.dll` com o sample carregando dxgi via interposer (ordem do Rift Apart). Cada combinação verifica no `ReShade.log` as linhas de contrato (gate aberto, feature criada, N frames apresentados) e falha o gate se faltar.
**Ganho esperado:** a ordem de load de qualquer jogo vira um caso reproduzível nesta máquina em segundos. Regressão de admissão pega antes da release. Não prova engines; prova a cadeia.

### 3.2 Regressão por imagem (M/G)

Script em `mfgdiagnostics` captura N frames reais e gerados (screenshot a cada Present, com `DLSSG.MultiFrameIndex`) e o PresentMon; compara por SSIM com referência por jogo e multiplicador (Cyberpunk, Onimusha, Crimson Desert). Limiar por jogo; falha vira artefato para olhar, não bloqueio.
**Ganho esperado:** o "parecia igual a olho" vira número. Protege retarget, correção temporal e variantes de kernel. Custo: cerca de 1 minuto por combinação no gate; referências refeitas quando o jogo atualiza.

### 3.3 Matriz publicada e template de issue (P)

`docs/compat/matrix.md` gerado de um `compat.json` (backend, API, GPU, SL, provider, engine, estado, fonte e data do relato). Template de issue pede o relatório de suporte colado inteiro; o script lê e sugere a linha. A lista do Calledme_kev entra como primeira fonte.
**Ganho esperado:** o README para de ser editado à mão. Zero efeito em runtime.

### 3.4 Sem cobertura, só documentado

Laptops híbridos (precisa de relato) e EAC (bloqueia o ReShade, não o addon).

## Bloco 4: base

| Item | O que muda | Esforço | Ganho esperado |
|---|---|---|---|
| 4.1 CI mínima | GitHub Actions, Windows, VS 2022: build do addon e os testes sem DLL de jogo (cerca de 40 dos 55) | M | build quebrado e regressão de unidade pegos no push |
| 4.2 Dependências | Streamline 2.14.16 e NGX SDK atuais (hoje 2.12.0 e 1.5); Detours/ReShade/Streamline por FetchContent ou submódulo | M | structs novas visíveis; onboarding sem o caminho fixo `renodx-build` |
| 4.3 Dividir `addon_overlay.inl` | uma unidade por página (General, Quality, Latency, Diagnostics, Support); pré-requisito do bloco 5 | M, mecânico | compilação mais rápida; testes por página |
| 4.4 `.clang-format` | só arquivos novos; o legado fica | P | diffs menores em PR |
| 4.5 Configuração unificada | página do painel com a origem de cada valor (`MFGAdaUnlock.ini`, `ReShade.ini`, `profiles.json`) | M | menos "a chave não pegou" |
| 4.6 PIX uma vez no XMX | captura das barreiras por evaluate | P | só mede |

Ganho em FPS: nenhum, em todos. O Present já custa 16 a 26 µs.

## Bloco 5: idiomas do painel

Seis idiomas: `en` (base), `pt-BR`, `es`, `ja`, `ko`, `zh-CN`. Só o painel; o `ReShade.log`, o relatório de suporte e o `sl.log` ficam em inglês, porque são lidos por quem dá suporte.

### 5.1 Tabela de strings (G)

**O que muda:** `strings.hpp` com um `enum class Str` e uma tabela por idioma (`strings_en.inl`, `strings_pt_br.inl`, …), acessada por `T(Str::kX)`; chamadas `ImGui::Text("…")` viram `T(...)`. Os cerca de 900 literais visíveis dos cinco arquivos do painel migram depois do 4.3. Formatação com `%s`/`%d` mantida; strings com ordem de argumentos diferente por idioma viram `{0}`/`{1}`.
**Teste:** `ini_panel_parity_tests.py` ganha "toda chave do enum existe em todo idioma" e "nenhum literal visível restou fora da tabela" (grep nos arquivos do painel). Falta em um idioma cai no inglês, nunca em string vazia.

### 5.2 Seleção do idioma (P)

Padrão: o idioma do ReShade (chave de idioma em `ReShade.ini`, nome a confirmar no fonte do ReShade 6.8), com override `[DLSSG] Language=pt-BR` e um seletor na página Support. Sem reiniciar.

### 5.3 Fonte para chinês, japonês e coreano (M, o risco do bloco)

**Verificado:** o ReShade monta um atlas com uma fonte para toda a interface; o addon hoje define Cascadia Mono, que não tem CJK. Sem glifos no atlas, o texto vira quadrados.
**O que muda:** quando o idioma escolhido for `ja`, `ko` ou `zh-CN` e não houver fonte escolhida pelo usuário, o addon define a fonte do sistema correspondente (`YuGothM.ttc` ou `meiryo.ttc`; `malgun.ttf`; `msyh.ttc`), do mesmo jeito que faz com Cascadia Mono, e diz no painel que a fonte do ReShade mudou. Os intervalos de glifos do atlas dependem do idioma do ReShade; se o ReShade estiver em inglês com fonte CJK, é preciso confirmar no fonte do ReShade se o atlas inclui os intervalos CJK. Se não incluir, o painel avisa "defina o idioma do ReShade para …" e cai no inglês.
**Hipótese a confirmar antes de codar:** o atlas do ReShade 6.8 escolhe intervalos pelo idioma do ReShade, não pela fonte.

### 5.4 Produção das traduções

Primeira passada automática, revisada por falante nativo da comunidade (Opti Discord) antes da release; `pt-BR` revisado por você. Strings técnicas (DLSS-G, XMX, Streamline, nomes de chaves) ficam em inglês em todos os idiomas.

**Ganho esperado:** adoção nas comunidades de língua chinesa, japonesa, coreana e hispânica, onde hoje o painel em inglês limita relatos. Custo permanente: toda string nova entra em seis tabelas (o teste 5.1 impede esquecer).

## Ordem de execução

1. Bloco 1 inteiro (um commit por item, gate verde) → **build de teste 1** aos relatores.
2. Bloco 4.3 (dividir o overlay) e 4.4, porque o bloco 5 depende deles.
3. Bloco 2 (2.1, 2.3, 2.5; 2.2 atrás de chave) → **build de teste 2**.
4. Bloco 3 (3.1 primeiro; 3.2 e 3.3 em paralelo com o bloco 5).
5. Bloco 5 (5.3 verificado antes de 5.1; 5.1; 5.2; 5.4).
6. Bloco 4 restante (4.1, 4.2, 4.5, 4.6).
7. Gate completo no plano High performance, campo nos jogos instalados (Cyberpunk, Onimusha, Crimson Desert com XMX e DLSS-G; SL Sample com `SimulatePreAda`), notas em `docs/releases/1.10.0.md`, `Prepare-Release.ps1`.

## Perguntas abertas

1. Relator consegue rodar OW2, Keeper e Rift Apart no build de teste 1 com `StreamlineLog=1` (e a chave de registro no Rift Apart)?
2. Alguém tem `ReShade.log` de um dos 11 jogos que funcionam (controle positivo)?
3. RDR Remastered parcial fica para depois.

Respondidas em 2026-11-10: versão 1.10.0; chinês só simplificado; espanhol neutro; sem revisores (idiomas marcados "tradução automática").

## Registro

- 2026-11-10: plano escrito (1.9.1 + 1.9.2 em fases); branch `feat/1.9.1` criada; nenhum código.
- 2026-11-10: decisão de release única com seis idiomas; plano reescrito como 1.10.0 em cinco blocos.
- 2026-11-10: aprovado; branch renomeada para `feat/1.10.0`; início pelo item 1.1.
- 2026-11-12: bloco 2 (2.1, 2.3) com o oráculo `SimulatePreAda=2` no SL Sample 2.14.16; o rastro mostrou que quem pergunta a arquitetura são as cópias renomeadas do app da NVIDIA (`190_E658700.dll`, `160_E658700.bin`), por isso a regra é por caminho. 2.2 e 2.5 ficaram de fora: o `sl.log` dos relatores decide 2.2.
- 2026-11-12: bloco 4.3 (páginas separadas; overlay legado removido) e bloco 5: 917 strings em seis idiomas, seletor no rodapé, fonte CJK do sistema, queda para inglês sem glifos; ferramentas `extract_panel_strings.py` e `sync_panel_tables.py` com `--check` no gate. Pendentes: bloco 3.2/3.3, bloco 4.1/4.2/4.4/4.5/4.6.
- 2026-11-10: bloco 1 escrito (1.1 a 1.8). Diferenças em relação ao plano: a linha de sistema sai no primeiro `init_device`, não no `DllMain` (ler o adaptador pode carregar `dxgi.dll`, proibido sob o loader lock); o card "System check" fica na página General ao lado do cartão da cadeia, fechado por padrão, e HAGS desligado vira um card de problema; `get_color_space` passou a 4 Hz em vez de "só na troca"; `DEVICE_REMOVED` só loga e para o XMX, sem desarmar outros hooks.
