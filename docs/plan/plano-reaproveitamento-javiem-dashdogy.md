# Plano: o que aproveitar do JavieEm e do dashdogy/MFG-Unlocker

Data: 2026-11-12. Estado: **dashdogy aprovado (A, B, C, D4, E) para a 1.10.0**, decisão do usuário: tudo na 1.10.0. JavieEm: investigação na seção 6, sem aprovação de porte.

Fontes analisadas:

- **JavieEm** (`C:\Users\ayase\Downloads\JavieEm`): fork do RTX40MFG (ImDreamt), com `source/` (~88 mil linhas sem third_party) e um `Release/version.dll` (não executado, não carregado).
- **dashdogy/MFG-Unlocker**, `main` em `128d081` (v1.7.1, publicada hoje). Já vendorizamos partes dele: o XeMFG/XMX em `a4cf2c4`, e o xvk e o motor de pacing em `b97deee`, que é a v1.7.0 (`src/addons/mfgunlock/xemfg/vendor/PROVENANCE.md`).

## 1. Licença: o que pode entrar no nosso código (MIT)

| Origem | Licença | Pode copiar? |
|---|---|---|
| dashdogy, código do projeto | MIT (Michael Robles) | **Sim**, com o aviso (já temos `LICENSE.dashdogy.txt`) |
| dashdogy, programas DXIL/SPIR-V derivados da Intel | termos da Intel | Só como hoje: extraídos localmente do DLL do usuário, sem commit |
| JavieEm, arquivos com `SPDX-License-Identifier: MIT` | MIT | **Sim**: `game_input_profiles.h`, `force_fg_input_policy.h`, `xefg_exit_lifecycle.h`, `tlou_legacy_camera.h`, `ngx_exporter_policy.h`, `xmx_policy.h`, `xmx_runtime_selection.h`, `dlssg_preset_config.h` |
| JavieEm, o resto (`patcher.cpp`, `midpoint_fix.cpp`, `hud_overlay.cpp`, `in_game_menu.cpp`, `xefg_backend.cpp`, `xefg_vulkan_bridge.cpp`, ...) | GPL-3.0 (o `COPYING` da raiz; sem cabeçalho próprio) | **Não**. Copiar torna o addon GPL. Só dá para usar como referência de ideias |
| JavieEm `xefg_pacing.h` | GPL-3.0, adaptado do OptiScalerDp4aUnlock | **Não**. Só como ideia |

Um detalhe: o XMX do JavieEm foi portado **do nosso repositório** (`xmx/PORT_PROVENANCE.md` cita `mavismmg/MFGAdaUnlock-RenoDx`). Nessa parte não há nada novo para trazer de volta.

## 2. O que aproveitar, por prioridade

### A. RTX 30/20 com DLSS-G (dashdogy, MIT): ataca os bugs em aberto

**A1. Resposta do NGX `GetFeatureRequirements` quando ele falha durante o `slInit`.** *(maior ganho provável)*
O dashdogy documenta (`ampere_startup_requirements.h`) dois casos em que o Streamline descarrega o `sl.dlss_g` na RTX 30 antes de existir device, e depois nada o traz de volta:
- `0xBAD00005 InvalidParameter`: o driver (617.42, `_nvngx.dll` RVA 0x452B0) recusa o *project ID* quando ele não é um GUID com engine CUSTOM. Isso acontece sempre que o jogo passa um project ID ou uma versão de engine.
- `FeatureNotFound/NotSupported`: nenhum provider DLSS-G está mapeado ainda no `slInit` (nem a cópia do jogo nem a OTA do app NVIDIA).

O nosso `HookedRequirements` (`src/addons/mfgunlock/preada_admission.hpp:300-330`) só corrige o bit de adaptador quando a chamada **dá certo**. Nos dois casos de falha, o resultado passa direto. Essa é uma **hipótese concreta para Outer Worlds 2 e Keeper** ("plugin inerte"), ainda não provada. Ela se confirma ou cai com o `sl.log` dos builds de teste da 1.10.0.

Entrega, em duas etapas:
1. Diagnóstico no log e no relatório: código de retorno do requirements no `slInit`, se havia provider mapeado, e o teste do project ID contra a regra do driver (`ProjectIdPassesDriverCheck`).
2. Só com essa evidência: responder "suportado" quando há um provider elegível já preparado. Nunca inventar a feature fora disso, que é a mesma regra do dashdogy.

Teste: o oráculo `preada_admission_sl_sample` (SL Sample + `SimulatePreAda=2`) ganha casos com project ID inválido e com provider ausente.

**A2. O registro de requisitos dentro do provider.** Hoje reescrevemos só o export `NVSDK_NGX_GetGPUArchitecture` (0x190 → 0x160, `preada_admission.hpp:496-523`). O dashdogy também reescreve o registro `{0x0B, 0x190}` em `NVSDK_NGX_D3D12_GetFeatureRequirements` e em `NVSDK_NGX_VULKAN_GetFeatureRequirements` do provider, que o Streamline lê como arquitetura mínima do plugin. Já temos o cabeçalho vendorizado sem uso (`xemfg/vendor/ampere_provider_arch.h`). Falta a implementação de `FindRequirementsArchitecture` (está em `ampere_provider_arch.cpp`), mais testes com os providers 310.x que já usamos no gate.

**A3. Parar os retries quando a admissão falhou.** O dashdogy viu, no Doom: The Dark Ages, que o Streamline repete um Create recusado a cada frame, e a VRAM cresce até o crash. Por isso ele força FG Off (`HoldOffWithoutAdmission`). Precisamos conferir se o nosso caminho pré-Ada tem o mesmo laço. Se tiver, aplicar a mesma política e dizer isso no painel.

**A4. Providers 310.1 a 310.6 na RTX 40.** Eles não têm a verificação de contagem/índice (`ngx_mfg_gate::Missing`; o caso do AION 2 com 310.6.0.0), então ficam em 2x. Hoje ficariam em 2x sem explicação. Entrega: linha de diagnóstico e aviso no painel ("este DLSS-G é antigo demais para mais de 2x; use o override do app NVIDIA").

**A5. Darktide (Streamline 2.12) preso em 2x na RTX 30.** O dashdogy corrigiu isso no próprio wrapper ("direct profile": o startup guarda `min(MultiFrameCountMax, 5)`). A nossa arquitetura é outra: o provider da NVIDIA com PTX retargetado. A entrega aqui é **investigação**, não porte. Precisamos ver se o nosso 2x tem a mesma causa, usando os logs do Darktide que já temos (memória `field-reports-2026-11-02`).

### B. Diagnóstico de crash (ideia do dashdogy; implementação nossa)

**B1. Registro de crash com minidump.** O dashdogy grava um registro e um `.dmp` ao lado do log (`fault_capture.*`). Nós não temos nada parecido: só existe um `AddVectoredExceptionHandler` local em `native_vulkan.cpp:800`. Ajudaria direto no crash de inicialização do Dying Light 2, que continua aberto. Seria opcional (`[Diagnostics] CrashDump=1`) e copiado pelos arquivos de suporte. Escrevemos a nossa versão, que é pequena: `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` carregado sob demanda, sem import estático, para manter o teste `xemfg_addon_imports`.

### C. XeMFG/XMX: atualizar o vendor para a v1.7.1 do dashdogy (MIT)

Merge de três vias contra `b97deee`, como o "plano J" da 1.7. O que muda entre a v1.7.0 e a v1.7.1, nas pastas que vendorizamos:
- `xess/cuda/provider_list.*` e `cuda_layers.h`: a rota de tensor cores do modelo portado, **incluindo RTX 30/20**. É o maior interesse para quem usa XMX numa 3060 Ti.
- `vulkan/xvk_tensor.*`: tensor no Vulkan.
- `sdk_pacing*`, `latency_pacing.h`, `pacing_diag.h`: pacing reescrito (AMD) e flip metering na NVIDIA só onde o driver diz que suporta.
- Fixes de `vk_host.cpp`, `xvk_dxgi.cpp`, `xvk_queue.cpp` etc.

Fica de fora: `ngx_provider*`, `vk_provider*`, `xemfg_provider_inprocess.cpp` (o "XeMFG como DLSS-G do OptiScaler"), que dependem do host do OptiScaler. Também DLSS 5, RTX Hair, Witcher dots e o menu próprio deles.

Risco: médio a alto. São muitos arquivos, e as nossas mudanças locais estão marcadas `MFGAdaUnlock 1.7`. Exige gate completo e teste de campo em pelo menos um jogo DX12 e um Vulkan.

### D. Ideias e dados do JavieEm (só a parte MIT, ou só a ideia)

- **D1. `tlou_legacy_camera.h` (MIT):** ABI do Streamline **1.0.x** (o TLOU Part I usa: sem `BaseStructure`, `Boolean` de 1 byte, funções que retornam `bool`). Só vale se o nosso caminho de entradas do XeMFG ler constantes do Streamline 1.x. A primeira tarefa é verificar isso.
- **D2. `game_input_profiles.h` (MIT):** convenções de profundidade por jogo. KCD2 com profundidade em escala 10000 e sem base de câmera sintética; TLOU com D24 normalizado e D32S8 achatado. Isso entra como dado nos nossos perfis de jogo do XeMFG (README, "Game profiles") só se tivermos relato desses jogos. Hoje não temos.
- **D3. `force_fg_input_policy.h` (MIT):** regra de que IDs de handle do NGX não são únicos entre dispatcher e provider. Comparar com o nosso caminho de entrada DLSS-SR; se tivermos a mesma ambiguidade, adotar a regra com um teste.
- **D4. Ideias de configuração** (implementação nossa):
  - `streamlineLogMode` = `inherit` / `off` / `default` / `verbose`, em vez do nosso `StreamlineLog=1` binário. É uma extensão pequena do `streamline_log.hpp` da 1.10.0.
  - Override de profundidade invertida (`auto` / `true` / `false`) para XeMFG/XMX.
  - Marcadores visuais de frame gerado/falho para depuração do XeFG.
- **D5. Não aproveitar:** o pacing do XeFG (GPL, offsets fixos do `libxess_fg`), o overlay e o menu próprios, o downloader de plugins via PowerShell e o proxy `winmm`/`version.dll`. Como addon ReShade, não precisamos de proxy.

### E. Documentação e compatibilidade

- **E1.** O `docs/compatibility.md` do dashdogy traz relatos de jogos com outra ferramenta. Não dá para importar como "funciona com MFG Unlock", mas dá para usar como **lista de jogos a testar** e como dica de conflito de mods (por exemplo: Cyberpunk com CET/RED4ext, RE Engine com REFramework).
- **E2.** Na nota de release e no PROVENANCE, registrar o commit de origem de cada arquivo portado.

## 3. Proposta de release

Recomendo **não** misturar isto com a 1.10.0, que já está no gate final:

| Release | Itens | Por quê |
|---|---|---|
| **1.10.0** (atual) | Só **A1 etapa 1** (diagnóstico do requirements e do project ID) | Cabe no tema "diagnóstico RTX 30" e faz os builds de teste já responderem à hipótese do OW2/Keeper |
| **1.11.0** | A1 etapa 2, A2, A3, A4, B1, D4 (`streamlineLogMode`) | Correções RTX 30 com evidência de campo, mais diagnóstico de crash |
| **1.12.0** | C (vendor v1.7.1), D1-D3 conforme a verificação | Merge grande e independente, com teste de campo próprio |

Cada item: um commit, gate verde, sem push nem tag sem você.

## 4. Ganhos esperados (honestos)

- **A1/A2:** podem destravar o DLSS-G na RTX 30 em jogos onde o plugin hoje fica inerte (OW2, Keeper). **Não é garantido.** É a melhor hipótese que temos, e o diagnóstico da etapa 1 diz se é essa a causa.
- **A3/A4:** evitam um crash por VRAM e trocam um "2x sem explicação" por uma mensagem clara.
- **B1:** um crash de inicialização (DL2) passa a ter um dump analisável, em vez de nada.
- **C:** XMX mais rápido na RTX 30/20 (tensor cores) e pacing melhor. O tamanho do ganho só se mede em campo.
- **D:** ganhos pequenos e pontuais. D1 e D2 só valem com relato de TLOU/KCD2.

## 5. Perguntas

1. Concorda em pôr só A1 etapa 1 na 1.10.0 e deixar o resto para 1.11.0/1.12.0? Ou prefere tudo na 1.10.0, como foi com os idiomas?
2. O item C (merge do vendor para a v1.7.1) vale o risco agora? Ou esperamos a próxima versão do dashdogy estabilizar?
3. O `Release/version.dll` do JavieEm: quer que eu inspecione estaticamente (versão, exports, hashes, sem carregar)? Ou só o código-fonte importa?

## 6. Investigação: a oferta do JaviEM ("xefg-vulkan bridge com XMX" e FG em jogos sem FG nativo)

O que o código dele faz (lido em `source/native`, sem executar nada):

| Recurso | Como funciona no JavieEm | O que temos hoje |
|---|---|---|
| **Ponte Vulkan → D3D12** (`xefg_vulkan_bridge.*`, 2,5 mil linhas) | Copia as imagens que o jogo marca no Streamline (Vulkan) para texturas D3D12 compartilhadas, roda o XeFG **D3D12** com fence compartilhado, e apresenta numa swap chain **DXGI** criada para a janela do jogo. Exige uma swap chain WSI e fila gráfica = fila de apresentação | xvk (dashdogy): roda os programas do XeFG traduzidos para SPIR-V **dentro** do Vulkan do jogo e apresenta pela WSI. Funciona no Indiana Jones; no RDR2 Vulkan o pacing é ruim |
| **XMX no Vulkan** (`xmx_vulkan*.h`) | Com a ponte acima, o XeFG D3D12 recebe o modelo 16 portado. Como o XMX vira "XeFG D3D12", não precisa do avaliador privado do NGX | XMX só com DLSS-G em DX12 (README: "XMX is not expected to work here: it needs DLSS-G in DX12") |
| **Force FG com DLSS-G nativo** (`forceFG`, ~640 referências em `patcher.cpp`) | Se o `slInit` do jogo pede DLSS SR mas não DLSS-G, acrescenta DLSS-G em `featuresToLoad`. Reaproveita as tags da avaliação do SR e chama `slSetFeatureLoaded`/`slDLSSGSetOptions` por conta própria. Em jogos que usam NGX sem Streamline, **cria uma sessão Streamline própria** (D3D12 e Vulkan) e cuida do Reflex | Não temos DLSS-G forçado |
| **Force FG com XeFG/XMX** | Entradas do SR (NGX D3D12/Vulkan, FSR2/3, XeSS) num registro comum (`fg_input_capture.h`) | XeMFG com a entrada DLSS-SR em DX12 (RDR2 funciona). XMX não usa essa entrada |
| Perfis por jogo | KCD2, TLOU Part I, Forza (~450 linhas específicas): câmera sintética, escala de profundidade, reconstrução "deep rebuild" do TLOU | Perfis de jogo do XeMFG, só por dados |

### O que ganharíamos

1. **XMX em jogos Vulkan** (Indiana Jones, Doom: The Dark Ages, RDR2 Vulkan, Enshrouded). Hoje esses jogos só têm XeFG.
2. **XMX em jogos sem FG nativo, só com DLSS-SR** (RDR2 DX12, KCD2, TLOU Part I). Hoje esses jogos só têm XeFG.
3. **DLSS-G de verdade (RTX 40/50) em jogos que têm DLSS SR mas não FG.** É o maior ganho de qualidade: o gerador da NVIDIA no lugar do XeFG.
4. Uma segunda rota Vulkan, alternativa ao xvk, para jogos onde o xvk falha. Ela apresenta por DXGI, ganha o flip model e o flip metering via NVAPI, e não traduz shaders.

### O que poderíamos ter de regressão (e por quê)

1. **Conflito com o ReShade, alto.** Somos um addon do ReShade. A ponte troca a apresentação Vulkan do jogo por uma swap chain DXGI própria. O ReShade continua enganchado no `vkQueuePresentKHR` do jogo, não na swap chain nova. Então os efeitos e o **nosso painel** podem sumir ou ser desenhados no frame errado. Isso também afeta HDR, captura e PresentMon. O código dele trata o ReShade como "overlay externo" a evitar (`patcher.cpp:17234`, `:24965`).
2. **Ordem de carga.** O force FG precisa editar o `slInit` **antes** do jogo chamá-lo. Nós só enganchamos o `slInit` quando o ReShade carrega cedo; é a mesma causa do Rift Apart. O JavieEm é um proxy `version.dll`, que carrega antes de tudo. No nosso modelo, o recurso falharia em silêncio em parte dos jogos.
3. **Sessão Streamline própria em jogos só-NGX.** Isso põe o proxy DXGI do Streamline entre o jogo e o ReShade, que também envolve a DXGI. A ordem de wrappers muda, com risco de crash ao iniciar, tela preta ou alt-tab quebrado. Esse risco atinge **todos** os jogos onde o recurso estiver ligado, não só os novos.
4. **Qualidade nos jogos forçados.** O DLSS-G espera câmera, HUD-less, UI e Reflex que esses jogos não mandam. Ele sintetiza câmera (near/far/FOV fixos) e usa perfis por jogo. Fora de KCD2/TLOU, a validação registrada é só "User confirmed KCD2 and TLOU correct" (`tests/fixtures/validated_games_91eccda.json`). Esperar artefatos de HUD e de cena em outros jogos.
5. **Manutenção.** São ~450 linhas específicas de jogo dentro de um monólito de 36 mil linhas, com histórico "V16.10.x". Portar é reescrever, e cada atualização dele vira um merge manual.
6. **Memória e restrições da ponte.** As texturas compartilhadas por frame aumentam a VRAM (o próprio código conta `sharedImportBytes`). A ponte só aceita uma swap chain WSI ativa e fila gráfica = fila de apresentação, o que recusa jogos com fila de apresentação separada.
7. **Licença.** O que não tem cabeçalho MIT é GPL-3.0 pelo `COPYING` da raiz, o que inclui `xefg_vulkan_bridge.cpp`, `xefg_backend.cpp` e `patcher.cpp`. O JaviEM **pode** relicenciar como MIT o que **ele** escreveu, mas não o que veio de outros: o `xefg_pacing.h` vem do OptiScalerDp4aUnlock (GPL). Sem uma declaração escrita dele, não dá para trazer o código.

### Recomendação

- **Aceitar a oferta, com condições:** uma declaração dele, por escrito (issue ou commit), de que a ponte Vulkan, o XMX Vulkan, o `fg_input_capture` e o force FG são de autoria dele e liberados sob MIT; e a lista do que veio de terceiros.
- **Fora da 1.10.0.** É uma rota nova de apresentação e de sessão, e o risco atinge jogos que hoje funcionam. A 1.10.0 já tem diagnóstico RTX 30, idiomas e os itens do dashdogy.
- **Protótipo em branch (`proto/xmx-vulkan-bridge`), em ordem de risco crescente, tudo desligado por padrão:**
  1. XMX com a **entrada DLSS-SR em DX12** (o nosso caminho atual, mas com XMX). Não precisa da ponte nem do force FG e não toca no ReShade.
  2. Ponte Vulkan → D3D12 como **terceira rota** de saída do XeMFG em Vulkan (`[XeFG] VulkanRoute=bridge`). Antes de tudo, testar com o ReShade e o painel ativos.
  3. DLSS-G forçado em jogos com Streamline SR, só com o addon carregado cedo; sem sessão Streamline própria.
  4. Sessão Streamline própria (jogos só-NGX): só se 1 a 3 derem certo em campo.
- Teste de aceitação de cada etapa: gate completo verde, **nenhuma mudança** nos jogos da matriz com o recurso desligado, e pelo menos dois jogos de campo com ele ligado.

## 7. Andamento (2026-11-14)

| Item | Estado | Evidência |
|---|---|---|
| A1 | Feito | O oráculo do SL Sample ganhou 3 casos. FeatureNotFound **sem** a resposta: DLSS-G desligado (mask 0x0), mesmo com a arquitetura respondida como Ada. Com a resposta: mask 0x1 e multi-frame 5, para FeatureNotFound e para InvalidParameter. Chave `[DLSSG] PreAdaRequirementsAnswer` |
| A2 | Feito | Registro `{0x0B, 0x190}` encontrado nos exports D3D11, D3D12 e Vulkan do provider do Sample e de duas versões do app NVIDIA; o oráculo mostra os 3 registros reescritos nos dois arquivos |
| A3 | Feito, sem teste de ponta a ponta | Política testada (`preada_caller`). O Sample não liga FG neste modo |
| A4 | Feito | Teste de versão (`runtime_version_tests`); mensagem no card da cadeia |
| A5 | Investigado, sem mudança | Não dá para reproduzir aqui: o override do app NVIDIA carrega o `sl.dlss_g` 2.14.16 até no Sample 2.11.1. Não há relato de Darktide em RTX 30 |
| B1 | Feito | `crash_record_tests`: um processo filho dá access violation; o teste confere o `.txt` (exceção, módulo+offset, endereço) e o `.dmp` |
| D4 | Feito | `StreamlineLog` = 0 NVIDIA, 1 detalhado, 2 padrão, 3 desligado |
| C | **Parado, decisão do usuário** | Ver abaixo |

### C: o tamanho real do merge para a v1.7.1

Merge de três vias, num rascunho, dos 31 arquivos vendorizados que mudaram upstream (base `b97deee`):
- 25 entram sem conflito.
- 6 têm conflito, 22 no total: `sdk_pacing.cpp` 11 (o arquivo cresce de 134 KB para 230 KB), `vk_host.cpp` 5, `xvk.h` 2, `cuda_route.cpp` 2, `xvk_dxgi.cpp` 1, `sdk_pacing.h` 1. É justamente onde estão as nossas mudanças locais (traces, flip metering, queue-ahead, as correções "MFGAdaUnlock 1.7" do Vulkan).
- As versões novas incluem cabeçalhos que não temos: `latency_pacing.h`, `latency_probe.h`, `pacing_diag.h`, `pacing_trace.h`, `frame_gen_target.h`, `diagnostic_paths.h`, `cuda_layers.h`, `xvk_tensor.h`, `host_xell_binding.h`, `xemfg_host_common.h`, e `vk_provider.h` (este é da rota do OptiScaler).
- **Mudou o formato dos payloads:** os programas da Intel, os SPIR-V e o fatbin CUDA agora vêm compactados em LZMA (`xess_programs.packed.h`, `xvk_programs.packed.h`, `xefg_cuda.packed.h`). Os programas também mudaram (tensor no Vulkan, camadas CUDA novas). Precisaríamos de um extrator novo, lendo o DLL da v1.7.1 com os novos hashes, mais o descompressor LZMA.
- Não há como validar aqui o ganho principal, a rota de tensor na RTX 30/20: precisa de uma placa dessas.

Recomendação: **não** pôr o C na 1.10.0. Fazer o C como release própria (1.10.1 ou 1.11.0), com os testes de campo de XMX que ele pede. Alternativa menor para a 1.10.0: trazer só correções pontuais, sem payload novo (por exemplo, o flip metering da NVIDIA só onde o driver diz que suporta), cada uma com seu teste.

### Correção na seção 6 (JaviEM)

O passo 1 recomendado (XMX com a entrada DLSS-SR em DX12) contraria uma decisão sua de 2026-11-02: "XMX with DLSS-SR-only games was dropped: latency is XMX's main pillar and an own presenter risks losing it". Esse passo sai da recomendação, a menos que você reabra a decisão. Os passos 2 a 4 continuam valendo como estavam.
