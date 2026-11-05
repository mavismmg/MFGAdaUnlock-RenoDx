# Plano da 1.8.3 — ciclo de vida dos hooks, XMX e relatos da comunidade

Status: **código pronto na `feat/1.8.3`, aguardando os testes manuais do usuário. O gate completo roda depois deles. Sem release.**

## Decisões de 2026-11-05

1. PR #2: incorporar o commit do onenowy, com crédito, e completar com R1 a R4.
2. Streamline anterior à 2.7: bloquear o multiplicador acima de 2x e avisar no painel (G1).
3. Crash em sequência: depois de 3 fechamentos inesperados seguidos, o card oferece o modo seguro com um clique.
4. cdb pode ser instalado para ler os dumps.
5. O `renodx-dlss5.addon64` pode ser movido temporariamente nas reproduções.
6. TLOU2: manter `DlssSuperResolutionPreset=13` para validar a correção.
7. Caminho para RTX 20/30 entra na 1.8.3; chave só de teste que simula GPU anterior à Ada aceita.
8. Sem pedir logs novos: a evidência é o que está em `Downloads\mfg-unlock-1.8.3`.
9. Escopo: tudo.

Base: `feat/1.8` em `3b8889f` (1.8.2). `fork/main` está em `9c46a11`, só com mudanças no README. PR pendente: #2 (`onenowy:fix/xmx-startup-lifecycle`, commit `68a2959`, base `main`).

Convenção deste plano: **Verificado** = provado por código, log ou reprodução nesta máquina. **Hipótese** = ainda sem prova, em ordem de probabilidade. Partes da análise do PR e do `Know_issues.txt` foram feitas com LLM; aqui elas só contam como verificadas quando há evidência própria.

## O que a investigação já provou

1. **Instância temporária do ReShade.** Nos três jogos da Nixxes, o ReShade carrega o addon, cria um device D3D12, descarrega o addon e carrega de novo. Os hooks instalados na primeira instância continuam no código do jogo depois do unload.
   - Ghost of Tsushima, `ReShade.log` de 2026-11-04 14:34: na linha 52, `architecture gates committed ... FNV1a64=0xdd8907b9b05a303f` para `160_E658700.bin`. Na linha 59, `xmx slot 0 hooked`. Nas linhas 79–84, `Unloading add-on "MFG Unlock"`. Na linha 105, a segunda instância registra `architecture gate refused: executable identity ...`.
   - O mesmo padrão aparece no TLOU Part II (2026-11-04 14:38 e 2026-11-05 08:29) e no HZD Remastered (suporte de 2026-11-03 17:39, linhas 50 e 79).
   - Causa da recusa: `modulesession::Inspect` calcula o hash de todas as seções executáveis ([module_session.hpp:25-78](../../src/addons/mfgunlock/module_session.hpp#L25-L78)), e `binarypatch::ProfileFor` exige esse hash exato. O jump do Detours que sobrou no `NVSDK_NGX_D3D12_EvaluateFeature` muda o hash.
2. **O mesmo defeito derruba o TLOU Part II.** Reproduzido nesta máquina em 2026-11-05 08:29, sem tocar no painel: o jogo fechou cerca de 65 s depois de abrir.
   - O WER registrou `0xC00000FD` (stack overflow) em `renodx-mfgunlock.addon64+0x2e240`. Com o PDB do build idêntico ao release (SHA-256 `8AA42B92…7AC6`), o offset cai em `HookedCreateFeature+0x30` ([addon_provider.inl:73](../../src/addons/mfgunlock/addon_provider.inl#L73)).
   - Os crashes de 2026-11-04 14:21:50 e 14:23:33 têm a mesma assinatura (`+0x2e88a` cai em `HookedCreateFeature+0x67a`, [addon_provider.inl:101](../../src/addons/mfgunlock/addon_provider.inl#L101)).
   - Mecanismo: `DlssSuperResolutionPreset=13` está salvo no `ReShade.ini` do TLOU2. Por isso a instância temporária instala o hook de `NVSDK_NGX_D3D12_CreateFeature` em `_nvngx.dll` ([addon_provider.inl:113-129](../../src/addons/mfgunlock/addon_provider.inl#L113-L129)), e o `DLL_PROCESS_DETACH` não o remove ([addon.cpp:1315-1351](../../src/addons/mfgunlock/addon.cpp#L1315-L1351)). A segunda instância é mapeada no mesmo endereço e engancha por cima. A trampolim nova executa o jump antigo, que cai de novo no `HookedCreateFeature`: recursão infinita na primeira criação do DLSS.
   - O log termina sempre em `DLSS Super Resolution preset 13 applied ... Streamline recreates the DLSS feature`.
3. **Os hooks do XMX têm o mesmo defeito.** Quando `generator::Evaluate` devolve `false` ([xmx_generator.hpp:544-553](../../src/addons/mfgunlock/xmx_generator.hpp#L544-L553)), `Slot<N>::Evaluate` chama o `evaluate` antigo e entra na mesma recursão ([xmx_hook.hpp:62-68](../../src/addons/mfgunlock/xmx_hook.hpp#L62-L68)). Isso é dedução do código e ainda não foi visto num dump.
4. **HZD:** `xmx init 2560x1440 format 24 flags 17 result -1000`. O GoT, com o mesmo tamanho e formato e `flags 1`, retorna 0.
5. **O build publicado já fixa o addon no primeiro Present.** Todos os logs trazem `engineering session is process-resident after first Present` ([addon.cpp:531-541](../../src/addons/mfgunlock/addon.cpp#L531-L541)).

Outro fator presente nos três jogos: o `renodx-dlss5.addon64` (DLSS 5 NR, "NGX hooks only") está instalado junto. Toda reprodução precisa rodar com ele e sem ele.

## Fase 1: PR #2 e o ciclo de vida dos hooks (prioridade máxima)

### 1.1 Revisão do PR #2

**O que muda**
- Remove `xmx::Attach(mod)` de `RememberDlssgModule` ([addon_provider.inl:302-304](../../src/addons/mfgunlock/addon_provider.inl#L302-L304)).
- Em `RunBoundedDiscoveryWorker`, que roda depois do primeiro Present real, calcula uma vez `attach_xmx = Enabled() && Preflight() && GetModuleHandleExW(PIN, self)`. A cada passada, depois de `RunProviderMaintenance()`, percorre `g_dlssg_modules` com `g_provider_maintenance_lock` exclusivo e chama `xmx::Attach` em cada módulo.

**Ciclo de vida antes do PR (1.8.2)**
- `DllMain` → `RunProviderMaintenance` → `DiscoverDlssgModules` → `RememberDlssgModule` → `Attach`.
- A thread do hook nasce bloqueada pelo loader lock e instala o hook logo depois que o `DllMain` termina.
- O ReShade descarrega a instância temporária, e o hook fica no provider.

**Ciclo de vida depois do PR**
- O provider pré-carregado só recebe o hook depois do primeiro Present, quando a instância já está fixada.
- O caminho do loader (`ProcessLoadedDlssgModule` → `Attach`, [addon_provider.inl:903-919](../../src/addons/mfgunlock/addon_provider.inl#L903-L919)) continua igual.

**Verificação independente da causa:** confirmada, com evidência mais forte que a do PR (itens 1 e 2 acima). A Witcher 3 não é o único caso: TLOU2, GoT e HZD falham pelo mesmo mecanismo.

**Auditoria**

| Ponto | Resultado |
|---|---|
| Deadlock por chamar `Attach` com o lock | Não encontrado. O lock interno do `Attach` é folha e é solto antes de `GetModuleHandleExW` e `CreateThread`. O callback do loader usa `TryAcquire` ([addon_provider.inl:905](../../src/addons/mfgunlock/addon_provider.inl#L905)). A ordem lock do provider → lock do loader já existe em `DiscoverDlssgModules`. Mesmo assim, o lock é desnecessário: basta copiar o vetor com o lock e chamar `Attach` fora. |
| Residência | O PIN do PR é redundante no build publicado (item 5), mas é inofensivo e torna o caminho independente da flag de engenharia. Se o PIN falhar, o XMX some sem log: falta uma linha de log. |
| Cobertura depois do worker | `attach_xmx` só roda dentro do worker (no máximo 10 s). Um provider achado por varredura depois disso, nas chamadas de `OnInitDevice` ou `OnInitCommandQueue`, não recebe hook. A 1.8.2 cobria esse caso. Regressão pequena: a NVIDIA passa a gerar, e o painel mostra "NVIDIA is generating". |
| Janela inicial | O worker dorme 250 ms antes da primeira passada. Os primeiros Evaluate vão para a NVIDIA e depois o XMX assume. Precisa de teste: troca no meio do jogo e status do painel. |
| Caminho do loader | **Continua com o defeito:** um provider carregado enquanto a instância temporária existe ganha um hook que sobra depois do unload. |
| Hook de preset | **Fora do PR, e é o crash provado do TLOU2.** |
| Corrida na mesma instância (**hipótese**) | O comentário em [loadhook.hpp:24-28](../../src/addons/mfgunlock/loadhook.hpp#L24-L28) diz que o callback roda com o loader lock. Na prática, `Notify` roda depois que o `LoadLibraryExW` real retorna ([loadhook.hpp:135-149](../../src/addons/mfgunlock/loadhook.hpp#L135-L149)), em geral sem o lock. A thread do `Attach` pode gravar o jump antes de `Inspect` terminar o hash, e o gate seria recusado mesmo sem instância temporária. |
| D3D11 | O XMX só se aplica ao D3D12 ([xmx_hook.hpp:70](../../src/addons/mfgunlock/xmx_hook.hpp#L70)), mas um device D3D11 temporário também carrega e descarrega o addon. O GoT abre o `crs-video.exe` só com D3D11. O defeito do preset vale para qualquer API. |
| Vulkan | Um provider renomeado sem entrada D3D12 consome um slot e é pulado ([xmx_hook.hpp:118-121](../../src/addons/mfgunlock/xmx_hook.hpp#L118-L121)). O slot é gasto antes da validação, e o GoT já usa 3 dos 4. |

**Veredito: aprovar com mudanças.** A ideia (só instalar hook depois da residência) está certa, mas o PR cobre um caminho só.

### 1.2 Correção proposta (R1 a R4)

- **R1, regra única:** nenhum hook que não pode ser desfeito antes da residência. Isso vale para os slots do XMX e para o `CreateFeature` do preset.
  - Antes da residência, o `Attach` só registra o módulo; o provider já fica retido pelo `modulesession`.
  - No primeiro Present, depois do PIN, instalar tudo o que ficou pendente, e também o que chegar depois, pelo loader ou pela varredura.
  - Incorpora o PR com estas mudanças: copiar a lista fora do lock, logar falha de PIN e cobrir a varredura depois do fim do worker.
- **R2, defesa em profundidade:** no unload explícito (`lpv_reserved == nullptr`), remover os slots do XMX e o hook de preset com `hook::Uninstall`, como já se faz com o `loadhook`.
- **R3, proteção contra recursão:** antes de enganchar, se a exportação já começar com um jump para dentro da imagem deste addon (sobra de instância morta), não enganchar de novo. Registrar "restart the game" e mostrar o motivo no painel.
- **R4, ordem no caminho do loader:** aplicar os gates antes de criar a thread do hook. Registrar os bytes do XMX como código próprio (`RecordCode`), para que um `Validate` posterior não recuse.
- **Alternativas consideradas:**
  - Mascarar o prólogo das exportações no hash. Descartada como correção principal: não resolve a recursão e afrouxa a verificação de identidade.
  - Fixar o addon já no `DllMain`, como faz o XeMFG. Descartada: fixaria também as instâncias temporárias, e o comentário em [addon.cpp:516-520](../../src/addons/mfgunlock/addon.cpp#L516-L520) documenta por que isso foi evitado.
- **Risco de regressão:** médio. Depende de o primeiro Present acontecer antes do primeiro `CreateFeature` do DLSS em jogos com preset. O retry no Present já existe para o KCD2 ([addon_provider.inl:131-143](../../src/addons/mfgunlock/addon_provider.inl#L131-L143)).
- **Testes automatizados:** fixture com duas instâncias da DLL de teste no mesmo processo (carregar, enganchar, descarregar, carregar de novo). Critérios: sem recursão, gate aplicado na segunda instância e nenhum hook antes da residência.
- **Validação, RTX 4070 SUPER, D3D12:**
  - TLOU2 com preset 13: abre, o gate é aplicado e o menu mostra o MFG.
  - GoT e HZD: o gate é aplicado.
  - Titans of the Tide (três instâncias), via log de quem relatou.
  - Dying Light 2: o `.bin` pré-carregado, motivo da 1.8.2.
  - Cyberpunk e Crimson Desert: regressão.
  - Witcher 3: o caso do PR.
  - Com e sem o `renodx-dlss5`.

## Fase 2: crashes e falhas nos jogos instalados

### 2.1 GoT e TLOU2: crash ao forçar o multiplicador do XMX
- **TLOU2 (verificado):** os três crashes registrados são o item 2 da seção de provas, e nenhum depende do multiplicador.
- **GoT:** WER de 2026-11-04 14:30:49, stack overflow em `windows.storage.dll`, seguido de AV em `sl.dlss_g.dll` 2.4.0+0x25560 no encerramento. O `RenoDX-DLSS5-crash.log` registra um AV de escrita em `0x100000007`, em `sl.dlss_g.dll+0x3437a`, aos 128,8 s.
  - **H1:** recursão pelo hook do XMX que sobrou (item 3 da seção de provas). Combina com o stack overflow.
  - **H2:** o GoT usa Streamline 2.4.0, anterior ao MFG. O override de `GetState` só atua com `DLSSGState` v2 ([framecount.hpp:2516](../../src/addons/mfgunlock/framecount.hpp#L2516)), mas o de `SetOptions` aceita `structVersion` de 1 a 5 e envia `numFramesToGenerate > 1` ([framecount.hpp:2171](../../src/addons/mfgunlock/framecount.hpp#L2171), [framecount.hpp:2250](../../src/addons/mfgunlock/framecount.hpp#L2250)). O `sl.dlss_g` 2.4 não foi feito para isso. Combina com o AV de escrita dentro do `sl.dlss_g`.
  - **H3:** interação com o DLSS 5 NR.
- **Investigar:**
  - Ler os dumps em `%LOCALAPPDATA%\CrashDumps` (GoT pid 22628, TLOU2 pids 20024, 20996 e 18172) com WinDbg. O leitor mínimo usado aqui não encontrou a pilha estourada.
  - Depois da Fase 1, forçar 3x e 4x no GoT com e sem o DLSS 5 NR, conferindo a linha `overriding DLSS-G numFramesToGenerate`.
- **Solução:**
  - Fase 1, que resolve H1.
  - **G1:** acima de 2x, o multiplicador forçado só é liberado com wrapper DLSS-G 2.7 ou mais novo (ABI com MFG) ou com a política de runtime substituto. Fora disso, o painel explica que o Streamline do jogo não passa de 2x. Vale para XMX e NVIDIA.
  - Alternativa: manter o override com aviso. Rejeitada se H2 se confirmar.
- **Validar:** GoT a 3x e 4x sem crash, ou bloqueado com mensagem clara; TLOU2 a 3x e 4x pelo menu do jogo.

### 2.2 TLOU2: as opções de MFG não aparecem com XMX
- **Verificado:** na instância real, o gate é recusado (2026-11-05 08:29:40, linha 99). O provider continua informando no máximo 1, o `sl.dlss_g` 2.7.2 repassa esse valor e o jogo só oferece 2x. Também falta a linha `advertising the verified Streamline ceiling`.
- **Solução:** Fase 1. Se o menu ainda não aparecer, conferir o perfil do teto do wrapper 2.7.2: o Titans of the Tide registra `stopped the DLSS-G plugin from lowering its compiled ceiling`, e o TLOU2 não.
- **Validar:** o menu do TLOU2 mostra 3x e 4x com XMX na RTX 4070 SUPER.

### 2.3 TLOU2: o jogo não abre depois de um crash
- **Verificado:** o estado que persiste é `[RenoDX.MFGUnlock] DlssSuperResolutionPreset=13` (e também `DynamicMFG=1`) no `ReShade.ini`. Com ele, todo início do jogo cai no item 2 da seção de provas.
- **Contorno imediato para você:** `DlssSuperResolutionPreset=0`, ou "Game default" no painel, se der tempo de abrir antes do crash.
- **Solução:**
  - Fase 1.
  - **Proteção contra crash em sequência:** o marcador de sessão já existe ([support_files.hpp:44-54](../../src/addons/mfgunlock/support_files.hpp#L44-L54), card em [panel_ui.hpp:316](../../src/addons/mfgunlock/panel_ui.hpp#L316)). Depois de N fechamentos inesperados seguidos, o card oferece "Start without optional hooks next time" (preset do DLSS, XMX e override). Decisão sua: automático ou com um clique.
- **Validar:** TLOU2 com o preset salvo abre e chega ao jogo; um crash forçado faz o card aparecer no início seguinte.

### 2.4 HZD Remastered: o XMX não inicializa
- **Verificado:** o `Init` interno da XeFG retorna `-1000` com `flags 17` (bit 0 `DepthInverted` + bit 4 `MvecJittered`, [xmx_generator.hpp:366-367](../../src/addons/mfgunlock/xmx_generator.hpp#L366-L367)). Com `flags 1`, o GoT inicializa.
  - Correção do relato: o XMX não volta para o DLSS-G. Depois do hook, a NVIDIA não gera mais, e o XMX repete o quadro real ([xmx_generator.hpp:542-553](../../src/addons/mfgunlock/xmx_generator.hpp#L542-L553)).
  - Outro defeito: `state.failed` nunca é limpo, nem quando o tamanho muda.
- **Hipóteses:**
  - **H1:** o modelo multiquadro portado ou o `Init` interno (1.3.1.78) não aceita MV com jitter.
  - **H2:** no `InitParams` interno, o bit 4 não significa MV com jitter.
  - **H3:** algum campo que falta (escala de jitter ou heap).
- **Investigar:** no Streamline Sample com `MvecJittered=1`, comparar `flags 17` com `maxFrames` 1 e 5, `flags 1` e `flags 16`. Ler o `Init` no `libxess_fg` 1.3.1.78 (`docs/research/xemfg-modo-xmx-2026-11-01.md`). Do `dlssg-to-xefg-xmx` (GPL), só a ideia.
- **Solução, conforme o resultado:**
  - (a) Limpar o bit e deixar a XeFG compensar pelo jitter de `FrameData`.
  - (b) Usar o modelo 2x quando houver jitter.
  - (c) Remover o jitter num passe próprio.
  - Em todos os casos: mensagem clara no painel e `failed` recuperável na troca de contexto.
- **Risco:** artefatos de movimento se a compensação estiver errada.
- **Validar:** HZD gerando a 2x (o HZD usa Streamline 2.2.1, ver G1); comparação visual com o DLSS-G.

## Fase 3: GPU e recursos expostos

### 3.1 RTX 20 e 30: a opção de DLSS FG não aparece
- **Verificado no código:**
  - O gate só reescreve as comparações com `0x1b0` (Blackwell), e `0x1b0 → 0x190` só leva a Ada ao caminho Blackwell ([addon_provider.inl:236-256](../../src/addons/mfgunlock/addon_provider.inl#L236-L256)). As verificações de Ada continuam.
  - O XMX só substitui o `Evaluate`. Suporte, capacidade e `CreateFeature` continuam com a NVIDIA, e enganchar o `CreateFeature` falha com `PlatformError` ([xmx_hook.hpp:3-6](../../src/addons/mfgunlock/xmx_hook.hpp#L3-L6)).
  - Log da 3060 Ti (Titans of the Tide): gate aplicado e XMX enganchado, mas `Game request: Not observed`. O jogo nunca ativa o FG.
- **Hipóteses sobre onde trava, a confirmar com log:**
  - (a) `slIsFeatureSupported(DLSSG)` no `sl.common` recusa o adaptador.
  - (b) A capacidade de FG no NGX está em 0.
  - (c) O `CreateFeature` falha.
- **Desenho do caminho próprio:**
  - Arquivos novos (`xmx_preada_*`), ativos só quando todo adaptador NVIDIA for anterior à Ada, o XMX estiver ligado e o preflight passar.
  - Componentes:
    1. Resposta de suporte do Streamline só para o DLSS-G.
    2. Parâmetros de capacidade do NGX para o DLSSG.
    3. Feature DLSSG emulada (`CreateFeature`, `Evaluate` e `Release` atendidos só pela XeFG, sem executar código do provider da NVIDIA).
    4. Requisitos de Reflex e PCL.
  - Nenhuma linha dos caminhos de Ada e Blackwell muda. Um teste falha se o gate de Ada ou Blackwell for tocado.
- **Etapas:** primeiro, um estudo de viabilidade no Streamline Sample com uma chave só de teste que simula GPU anterior à Ada nesta RTX 4070 SUPER; depois, campo com quem relatou (3060 Ti). Trabalho grande, provavelmente para a 1.9.
- **RTX 3070 + Crimson Desert + `dxgi.dll` + OptiScaler nightly + SM86 Mod 0.4.1 (hipóteses):**
  - Só um proxy pode ser `dxgi.dll`.
  - O OptiScaler também engancha o NGX e o FG (perfil `conflicting-frame-generation-plugins`).
  - O SM86 Mod altera o provider, então o hash de identidade recusa o gate.
  - Mesmo sem conflito, sem o caminho 3.1 não há FG numa RTX 30.
  - Pedir `ReShade.log`, `OptiScaler.log` e a lista de DLLs; documentar que a combinação não é suportada.

### 3.2 RTX 50 com DMFG pelo XMX
- **Verificado no código, ainda sem o log de quem relatou:**
  - O gerador lê `DLSSG.MultiFrameCount` a cada chamada ([xmx_generator.hpp:437](../../src/addons/mfgunlock/xmx_generator.hpp#L437)).
  - O formato do contexto não inclui a contagem, então mudar a contagem não recria o contexto.
  - Na RTX 50, o provider fica intocado ([addon_provider.inl:390-407](../../src/addons/mfgunlock/addon_provider.inl#L390-L407)). O runtime da NVIDIA conduz o DMFG, e o XMX gera o que for pedido, até 6x.
  - Conclusão: é uma capacidade existente que funciona por construção, não um caminho acidental.
- **Generalizar para a RTX 40:**
  - Depende de o runtime informar suporte dinâmico com o gate aplicado (`slDLSSGGetState confirms NVIDIA Dynamic MFG support`).
  - O modelo da XeFG precisa aceitar `outputCount` variável.
  - Testar num jogo com a pilha "Dynamic MFG release stack" (Streamline 2.14.16 / 310.9.16), depois da Fase 1, medindo latência por PCL.
  - Nada muda no caminho da RTX 50.

## Fase 4: estabilidade e relatos da comunidade (evidência antes de código)

| Item | O que se sabe | Hipóteses | Próximo passo |
|---|---|---|---|
| **Once Human** (RTX 4080 Laptop, 1.8.2) | XMX: gate recusado antes do hook, porque nem o provider do jogo (`0x681d30ce`) nem o do DriverStore têm perfil. `numFramesToGenerate=2` foi recusado com `sl::Result 38`, então só 2x. XeMFG: sessões de 40 s; fila de flip metering ligada; `game_fg_disabled` e `source_wait_not_completed`. **Verificado:** o addon só grava `MFGAdaUnlock.ini` e `ReShade.ini` ([config_store.hpp:31](../../src/addons/mfgunlock/config_store.hpp#L31)), nunca a configuração do jogo. | H1: flip metering ou "Queue metered frames early" num notebook com Optimus/MUX. H2: o jogo tem XeSS-FG nativo e entra em conflito. H3: espera de fence. Presets: o próprio jogo reescreve ao ver a capacidade de FG ou Reflex mudar. | Pedir o arquivo de configuração do jogo antes e depois, o modo MUX e PresentMon (PCL) com e sem o mod. Adicionar perfil de provider para o build do Once Human. |
| **RDR2 "XMX-SR"** | O log do Rogallo (1.8.2, 2026-11-04) é do **XeMFG em Vulkan** (`vk_host_config standalone frames 2`), não do XMX: o RDR2 não tem DLSS-G. "Queue metered frames early" é uma opção do XeMFG ([native_controls.cpp:257](../../src/addons/mfgunlock/xemfg/native_controls.cpp#L257)); `vk_present_metering_config enabled:true`. | H1: o metering bloqueia com fila cheia. H2: custo da XeFG e cópias. H3: interação com o limitador do jogo. | PresentMon com PCL e GPU busy a 1x e 2x, com metering ligado e desligado, no RDR2 instalado aqui. Decidir o padrão do metering no Vulkan. |
| **Elden Ring + ERSS 5.0.1** | Não há log de XMX. O log do FusselBob é do XeMFG 1.6/1.7. | H1: o ERSS gera quadros por caminho próprio, não pelo DLSS-G do NGX (incompatível por desenho). H2: provider renomeado ou fora do alcance da varredura. H3: ordem com o proxy do ERSS. | Pedir `ReShade.log` com XMX e a configuração do ERSS; ver se algum `nvngx_dlssg` é mapeado. Você abre o jogo, com EAC desligado. |
| **PBZ + DualSense Edge/DSX** | **Verificado:** o caminho do XMX não tem nenhum hook de entrada. O único hook de entrada do código é o `WH_GETMESSAGE` do XeMFG ([native_window.cpp:58](../../src/addons/mfgunlock/xemfg/native_window.cpp#L58)). | H1: a mudança de pacing ou de CPU afeta o polling do PBZ ou do DSX. H2: bloqueio de entrada do overlay do ReShade. H3: problema conhecido entre DSX e PBZ. | Pedir teste com o addon em modo DLSS-G sem XMX e com o ReShade sem o addon. Prioridade baixa. |

## Fase 5: auditoria do XMX e do CUDA

| # | Achado | Situação | Ação |
|---|---|---|---|
| A1 | Hooks sem desfazer no `DETACH`: slots do XMX e preset do `CreateFeature`. Dos hooks do Detours, só o `loadhook` e o `framecount` são removidos. `TryInstallHooks`, `TryBootstrapVTable` e `RestoreParameterVTable` não têm chamada (código morto). | Verificado | Fase 1, e remover o código morto |
| A2 | O hash de identidade cobre todo código executável; qualquer hook estranho no provider recusa o gate | Verificado | Fase 1 (R4); mensagem própria no painel para "provider modificado" |
| A3 | Corrida no caminho do loader (thread do `Attach` contra `Inspect`) | Hipótese | Teste com fixture e R4 |
| A4 | Gerador global sem lock; `failed` permanente; `logs++` não atômico; slot gasto antes do PIN e da validação (`kSlots=4`) | Verificado no código | Isolar o estado, `failed` recuperável, slot só depois da validação |
| A5 | O preflight só verifica o timestamp do `libxess_fg`; a versão do `libxell` não é verificada (o TLOU2 tem a 1.3.0.5 do jogo). `GetModuleHandleW` prefere a cópia que o jogo já carregou. | Verificado | Verificar a versão do `libxell` usada no modelo multiquadro e logar o caminho efetivo |
| A6 | Estados de recurso fixos (`kInputState = COPY_DEST` para profundidade e movimento) | Hipótese, já no item 11 da 1.8.1 | D3D12 debug layer e GBV no Streamline Sample; NGX direto, sem Streamline |
| A7 | Override em Streamline anterior à 2.7 | Hipótese H2 de 2.1 | G1 |
| A8 | CUDA e XeMFG: congelamento no Vulkan (driver), pacing do RDR2, matriz de latência, travadas no Once Human | Pendente de planos anteriores | Fase 4; nada muda sem medição |
| A9 | DLSS 5 NR instalado junto em todos os jogos da Nixxes | Verificado | Toda reprodução com e sem ele; documentar se houver conflito |

## Ordem de execução sugerida

1. Fase 1 (R1 a R4, com o PR incorporado) e os testes de duas instâncias. Resolve o item 1.1 e, pelos logs, os itens 2.2 e 2.3, provavelmente também a H1 do 2.1.
2. G1 e a proteção contra crash em sequência (itens 2.1 e 2.3).
3. HZD `-1000` (item 2.4), começando pelo Streamline Sample.
4. A2 a A6.
5. Coleta de evidência da Fase 4 e do item 3.2, em paralelo, porque depende de terceiros.
6. Estudo de viabilidade do caminho anterior à Ada (item 3.1), provavelmente na 1.9.

Bateria completa no plano High performance com o monitor ligado, restaurando o Balanced no fim. Campo numa sessão só: TLOU2, GoT, HZD, Cyberpunk, Crimson Desert e Witcher 3.

## Perguntas abertas

1. PR #2: peço mudanças ao onenowy, ou incorporamos o commit dele, com crédito, e completamos com R1 a R4?
2. Forçar acima de 2x com Streamline anterior à 2.7 (GoT 2.4, HZD 2.2.1): bloquear (G1) ou manter com aviso?
3. Proteção contra crash em sequência: modo seguro automático depois de N fechamentos, ou só oferecer com um clique? Qual N?
4. Posso instalar o Debugging Tools for Windows (cdb) para ler os dumps do GoT e do TLOU2?
5. Posso mover temporariamente o `renodx-dlss5.addon64` nas pastas dos três jogos para as reproduções?
6. TLOU2: quer que eu mude `DlssSuperResolutionPreset` para 0 no seu `ReShade.ini`, ou prefere manter o estado para validar a correção?
7. O caminho para RTX 20/30 entra na 1.8.3 ou fica para a 1.9? Aceita uma chave só de teste que simula GPU anterior à Ada?
8. Pedir logs a quem relatou: RTX 50 com DMFG, Elden Ring com ERSS, PBZ com DSX, RTX 3070 e Once Human (arquivo de configuração do jogo).
9. Escopo da 1.8.3: só Fases 1 e 2 e o item 2.4, ou também a auditoria?

## O que mudou em relação ao plano

- **R1:** além do PIN do PR, o addon é fixado no callback do primeiro Present real (`EnsureResident`). O bloco do PR no worker foi substituído: o `RunProviderMaintenance` entrega os providers ao XMX depois dos gates, o que também cobre as varreduras depois do fim do worker. O commit do onenowy entrou com a autoria dele.
- **R2:** a remoção no unload existe, mas na prática é inalcançável, porque os hooks só entram depois do PIN.
- **R3:** um hook que já leva para a imagem do addon é recusado em `hook::Install` e `InstallOne`. Os detours do XMX e do preset devolvem falha em vez de chamar uma trampolim nula.
- **R4:** a thread do XMX cria a sessão do provider (hash da identidade) antes de enganchar e registra os bytes do hook como código próprio.
- **2.1 GoT:** o dump (lido com cdb) não tem nenhum frame do addon. É uma exceção C++ dentro do `sl.dlss_g` 2.4.0, e o tratador de exceções do `sl.interposer` recursa até o stack overflow. A H1 (hook órfão) foi descartada para o GoT; a H2, coberta pelo G1, é a que sobra.
- **2.2:** o `sl.dlss_g` 2.7.2 do TLOU2 não tem o padrão do teto conhecido. O menu depende do provider com o gate aplicado; validar em jogo.
- **2.3:** o marcador de sessão passou a contar fechamentos inesperados seguidos e só termina na saída do processo. O modo seguro é um arquivo `MFGAdaUnlock.safe-mode` adotado pelo pid do próximo processo. O XeMFG mostra o aviso sem a oferta.
- **2.4 HZD:** uma sonda offline mostrou que o modelo 2x da Intel aceita `flags 17`. O `-1000` vem do modelo multiquadro portado (exceção capturada pelo wrapper C da XeFG). Correção: nova tentativa com o modelo 2x da Intel; o HZD só oferece 2x de qualquer forma (Streamline 2.2.1).
- **3.1 (revisto em 2026-11-05, depois de estudar nefh/MFGAmpereUnlock-RenoDx e sdli1995/dlssg_for_sm86):** a primeira conclusão ("a recusa está fora do alcance do XMX") estava errada. Todo provider 310.x exporta `NVSDK_NGX_GetGPUArchitecture` = `mov eax, 0x190; ret`, o mínimo que o núcleo do NGX lê. Decisão do usuário: portar a admissão do nefh (MIT, com crédito), isolada, na 1.8.3, e o jogo deve oferecer o multiplicador pelo próprio menu, como na RTX 40/50. Entregue em `preada_admission.hpp`, ativo só com toda GPU NVIDIA anterior à Ada e XMX ligado:
  - mínimo do provider rebaixado para Turing, registrado como código próprio;
  - NVAPI responde Ada só dentro de `slIsFeatureSupported` e do `GetFeatureRequirements` do FG;
  - as duas consultas admitem o adaptador;
  - o gate multiquadro é escrito em Turing em vez de Ada.
  
  A recompilação de PTX do nefh não entrou, porque o XMX não roda os kernels da NVIDIA. Se o `CreateFeature` falhar numa RTX 20/30 real (módulos CUDA sm_89 na criação), o painel mostra o motivo e oferece o XeMFG, e o próximo passo seria portar essa recompilação. Simulado no Streamline Sample com `SimulatePreAda=1`; ainda sem placa real.
- **A6 (revisto):** em lista de compute, o estado gráfico da tag vira `NON_PIXEL_SHADER_RESOURCE`, o estado que o Streamline entrega ao NGX segundo as capturas do sdli. No Streamline Sample, os erros da debug layer caíram de 15.356 para 171 (só nos primeiros quadros), com geração.
- **3.2:** sem mudança de código. Na RTX 50, o XMX gera o que o runtime da NVIDIA pede a cada chamada; não foi generalizado para a RTX 40.
- **A1:** o código morto do patch de vtable do NGX ficou, porque o painel ainda lê os contadores dele e ele nunca instala hook.
- **A6:** a debug layer no Streamline Sample 2.14.16 mostrou cerca de 15 mil erros com XMX e 0 com o gerador da NVIDIA. O `Evaluate` chega numa lista de compute, enquanto os recursos do jogo estão em estados gráficos; a NVIDIA lê por CUDA. Correção parcial: estado da tag numa lista DIRECT. Em compute fica o comportamento da 1.8.0, porque nenhuma barreira é válida ali; a correção completa (cópia numa lista DIRECT própria com fence) fica para a 1.9.
- **Fase 4:** sem logs novos, por decisão. Virou documentação no README e nas notas.

## Testes manuais pendentes, numa sessão só

Build: `build/tests/xemfg/Release/renodx-mfgunlock-native.addon64` (1.8.3.0).

1. **TLOU Part II** com `DlssSuperResolutionPreset=13` salvo e XMX: abre, chega ao jogo, o log tem `architecture gates committed` na instância que fica e o menu mostra 3x/4x. Com e sem o `renodx-dlss5.addon64`.
2. **Ghost of Tsushima** com XMX: gate aplicado; forçar 3x no painel mostra o aviso do Streamline 2.4 e não fecha o jogo.
3. **HZD Remastered** com XMX: `the multi-frame model refused these inputs; Intel's 2x model runs instead` e gera a 2x.
4. **Regressão:** Cyberpunk 2077 e Crimson Desert com XMX (3x a 6x, Alt+Tab, troca de resolução); Witcher 3 com XMX (caso do PR); Dying Light 2 (provider pré-carregado).
5. **Modo seguro:** três fechamentos forçados seguidos (finalizar o processo), o card oferece o modo seguro, o próximo início mostra o card "Safe mode" e o seguinte volta ao normal.
6. **Pré-Ada simulado:** `[DLSSG] SimulatePreAda=1` com XMX num jogo (Cyberpunk): o log tem `pre-Ada provider minimum architecture 0x190 -> 0x160`, os gates aplicados e `frame generation feature was created`; o menu do jogo oferece 3x/4x e o XMX gera. Depois remover a chave. Numa RTX 20/30 real (quem relatou): o mesmo, e o menu deve mostrar o DLSS Frame Generation.
7. **Dynamic MFG na RTX 40 com XMX** (item 3.2), num jogo com Streamline 2.14.16: só observar e registrar.
8. **RDR2 em Vulkan com XeMFG:** "Queue metered frames early" ligado e desligado, PresentMon com PCL a 2x.

## Registro

- **2026-11-05 · implementação** Commits na `feat/1.8.3`: o do onenowy (`de8de31`), ciclo de vida dos hooks (`8d83359`), Streamline 2.7 (`c2fc9b6`), modo seguro (`7dbb337`), XeFG 2x no HZD (`6de60fc`), log do runtime (`f9ced61`), pré-Ada (`e807f08`), estados das tags (`9f0a24e`), versão (`67a890a`). Testes novos rodados isoladamente: `stale_hook`, `force_policy`, `support_files`, `gpu_arch`, `xmx_policy`, `ini_panel_parity`, `xemfg_version_resource`; todos passaram.
- **2026-11-05 · Streamline Sample 2.14.16** Com este build e XMX: `process-resident after first Present`, depois `installing on 3 module(s) mapped before the first Present`, depois os três slots enganchados; `init ... frames 5 result 0` e `execute result 0`. Debug layer: 15.356 erros antes da correção das tags; o gerador da NVIDIA, 0.
- **2026-11-05 · investigação** Logs de `Downloads\mfg-unlock-1.8.3` e das pastas dos jogos copiados para o scratchpad antes de qualquer execução. TLOU2 aberto uma vez pela Steam, sem interação: stack overflow em `HookedCreateFeature` aos ~65 s (WER 08:30:36). Offsets resolvidos com o PDB do build `renodx-mfgunlock-native` de 2026-11-02 23:13, idêntico ao release.
