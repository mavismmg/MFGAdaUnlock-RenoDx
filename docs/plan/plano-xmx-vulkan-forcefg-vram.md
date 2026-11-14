# Plano: XMX no Vulkan, FG em jogos sem FG nativo, e VRAM do XMX

Data: 2026-11-14. Estado: **aguardando aprovação**. Nada foi implementado.

## 1. Princípio: nada de GPL, e o XMX DX12 intocado

- **Sem código GPL.** É a mesma regra do XMX original (a ideia veio do dlssg-to-xefg, GPLv3; nenhum código foi copiado). Do JavieEm aproveitamos só **ideias e fatos públicos de API**: Streamline, NGX, Vulkan e DXGI. As implementações vêm do zero, ou do código MIT que já vendorizamos do dashdogy. Não leio mais os `.cpp` GPL dele para estes itens.
- **Resposta à sua pergunta: sim, o XMX DX12 poderia ficar instável**, se os itens novos mexessem no que ele usa:
  - o hook de `NVSDK_NGX_D3D12_EvaluateFeature` (`xmx_hook.hpp`);
  - o gerador D3D12 (`xmx_generator.hpp`), que espera os parâmetros e command lists do NGX D3D12;
  - o hook de `slInit` e as opções do DLSS-G (`framecount.hpp`), que todo jogo com DLSS-G atravessa.
  
  Por isso o plano **não toca na estrutura DX12**:
  1. **Arquivos separados.** O XMX Vulkan fica em módulo próprio e engancha só os exports `NVSDK_NGX_VULKAN_*`. Não inclui nem modifica `xmx_generator.hpp` nem `xmx_hook.hpp`.
  2. **Guarda automática.** Um teste novo falha se `xmx_generator.hpp` ou `xmx_hook.hpp` mudarem num commit destes itens (hash fixado). Só um commit que você aprovar pode atualizar o hash.
  3. **Desligado por padrão.** Cada item nasce desligado e só age onde o jogo **não** tem o caminho DX12 atual: jogo Vulkan, ou jogo sem DLSS-G. Num jogo DX12 com DLSS-G nativo, nada muda.
  4. **Testes do caminho DX12.** O gate completo roda em cada commit. Antes de qualquer release, um teste de campo de XMX DX12 (Crimson Desert ou Cyberpunk) compara com a 1.10.0.

## 2. XMX no Vulkan, melhor que a ponte do JavieEm

**A ponte dele:** copia as imagens para D3D12 e apresenta numa swap chain DXGI própria. O ReShade não vê essa swap chain, e com isso perdemos efeitos, HDR e o painel.

**A nossa rota:** usar o que já temos, sem trocar a apresentação.
- O XMX DX12 substitui o gerador da NVIDIA dentro do `EvaluateFeature` do DLSS-G. A NVIDIA continua apresentando e fazendo o pacing e o Reflex. O XMX Vulkan faz o mesmo no `NVSDK_NGX_VULKAN_EvaluateFeature`: grava o trabalho do modelo portado **no próprio command buffer da avaliação**, pelo xvk (a camada D3D12-sobre-Vulkan que já vendorizamos). A apresentação continua sendo do DLSS-G no Vulkan do jogo, e o ReShade continua vendo tudo.
- O dashdogy fez exatamente isso na v1.7.1, em MIT (`xess/vulkan/vk_provider.cpp`, "XeMFG as a Vulkan DLSS-G provider"): avalia gravando no command buffer da avaliação, sem submeter nada. Lá a entrada é o substituto de NGX do OptiScaler. Aqui a entrada seria o nosso hook nos exports Vulkan do `nvngx_dlssg`, como o XMX DX12 faz no D3D12.

Etapas:
1. Trazer `vk_provider.cpp` e o que ele precisa (MIT, depois do merge da v1.7.1 já feito) **sem registrá-lo no OptiScaler**.
2. Escrever o hook de `NVSDK_NGX_VULKAN_CreateFeature/EvaluateFeature/ReleaseFeature` do `nvngx_dlssg`, num módulo separado (`xmx_vulkan.cpp`), que chama o provider.
3. Chave nova `[DLSSG] XmxVulkan` (desligada por padrão) e uma linha no card da cadeia.
4. Teste de campo: Indiana Jones e Doom: The Dark Ages (Vulkan, DLSS-G nativo).

Riscos: o xvk precisa dos programas do modelo portado em SPIR-V. O dashdogy os tem, mas é preciso conferir que o nosso conjunto extraído os inclui. Também é preciso conferir a VRAM (seção 4).

## 3. FG em jogos sem FG nativo, melhor que o "force FG" do JavieEm

**O dele:**
- sintetiza dados de câmera com valores fixos;
- tem perfis por jogo dentro de um monólito;
- em jogos só-NGX, cria uma sessão Streamline própria.

**O nosso: usar o que o jogo já entrega ao Streamline.** Um jogo com DLSS SR via Streamline já manda ao Streamline tudo o que o DLSS-G precisa:
- as tags de profundidade e de vetores de movimento;
- as constantes de câmera por frame (`slSetConstants` é comum a todas as features);
- os marcadores PCL/Reflex, quando o jogo usa Reflex.

Então "forçar" é só **pedir o plugin e ligar a feature**, sem inventar câmera:
1. No nosso hook de `slInit`, se o jogo pediu DLSS SR e não pediu DLSS-G, acrescentamos `kFeatureDLSS_G` em `featuresToLoad`. Isso só funciona com o addon carregando cedo, e o painel já avisa quando não carrega.
2. Quando o jogo já está gerando quadros com DLSS SR, chamamos `slDLSSGSetOptions(eOn)` com o multiplicador escolhido.
3. Sem Reflex do jogo, ligamos o Reflex do Streamline (o DLSS-G exige) e seguimos o pacing dele. **Sem Reflex nem marcadores, o item recusa** e explica no painel. Não sintetizamos marcadores.
4. **Sem HUD-less, o HUD é interpolado**, como no DLSS-G de qualquer jogo sem HUD-less. O painel avisa.
5. Com isso, o **XMX também passa a funcionar nesses jogos**, porque substitui o gerador de um DLSS-G de verdade. Isso respeita a sua decisão de 2026-11-02 ("XMX com DLSS-SR descartado: latência é o pilar do XMX, e um apresentador próprio arrisca perdê-la"): aqui não há apresentador próprio, quem apresenta e faz o Reflex é a NVIDIA.
6. **Fora do escopo:** jogos que usam NGX sem Streamline (a "sessão Streamline própria" do JavieEm). É o maior risco de crash e de conflito com o ReShade, e para esses jogos o nosso XeMFG com a entrada DLSS-SR já existe.

Chave `[DLSSG] ForceFrameGeneration` (desligada por padrão; vale só para jogos cujo `slInit` não pediu DLSS-G, então jogos com FG nativo nunca são afetados).

Testes:
- **Automático:** o Streamline Sample em modo só-SR, se der para configurá-lo assim. Verificar que o plugin carrega e que "Multi-frame supported" aparece.
- **Campo:** RDR2 DX12 e um jogo UE5 com DLSS SR sem FG.

## 4. VRAM do XMX: o relato da RTX 4080 ("quase o dobro")

**Hipótese** (baseada no código, ainda sem medição):
- **O feature da NVIDIA continua alocado.** O XMX não pode enganchar o `CreateFeature` (ele falha com PlatformError, `xmx_hook.hpp:3-6`). A NVIDIA cria o DLSS-G completo e aloca o que precisa para gerar (fluxo óptico, ativações do modelo, históricos), e o XMX nunca usa isso.
- **O XeFG do XMX sempre nasce com capacidade para 5 quadros gerados.** É a regra de troca ao vivo do multiplicador ("Every portable context is created with five generated frames", `xess_policy.h`), mesmo jogando em 2x ou 3x.
- **Cópias das entradas.** O XMX copia profundidade, movimento, cor e HUD-less para recursos próprios (`xmx_generator.hpp:298`), e as cópias de uma resolução anterior ficam aposentadas até a GPU liberá-las.

Etapas, **medir antes de mudar**:
1. **Medição.** O addon já lê a VRAM do processo (`addon_latency.inl:574`). Mostrar no relatório de suporte o uso local antes e depois de ligar o FG, e o tamanho das cópias do XMX. Medir no mesmo jogo e no mesmo ponto: DLSS-G nativo, XMX em 2x e XMX em 4x.
2. **Opção de capacidade igual ao multiplicador** (`[DLSSG] XmxCapacity=multiplier`, desligada por padrão no começo). O XeFG nasce com os quadros do multiplicador escolhido e a troca de multiplicador recria o contexto (um soluço curto, só ao mudar). Ganho esperado: algumas saídas de quadro a menos (a 4K, cada uma tem dezenas de MB). O tamanho exato vem da medição.
3. **Cópias sob demanda.** Copiar só as entradas que o XeFG usa naquele modo (por exemplo, nada de HUD-less quando a composição de UI está desligada), e liberar as aposentadas assim que a fence da GPU passar.
4. **Fora do escopo, por enquanto: evitar a alocação da NVIDIA.** Seria preciso impedir o `CreateFeature` de alocar sem quebrá-lo. Isso exige engenharia reversa do provider, com risco direto ao XMX DX12, o que contraria a seção 1. Fica registrado como pesquisa, se a medição mostrar que é a maior parte do custo.

Este item **mexe no XMX DX12** (opção 2 e cópias sob demanda): é a única exceção à seção 1, e por isso entra opcional e medido, só com a sua aprovação.

## 5. Ordem proposta

| Ordem | Item | Toca no XMX DX12? | Precisa de campo |
|---|---|---|---|
| 1 | VRAM: medição no relatório | Não (só leitura) | Sim, 1 jogo |
| 2 | VRAM: capacidade = multiplicador, cópias sob demanda | **Sim, opcional** | Sim |
| 3 | XMX Vulkan via provider do dashdogy | Não (módulo separado) | Indiana Jones, Doom TDA |
| 4 | DLSS-G forçado em jogos Streamline só-SR | Não (só jogos sem DLSS-G) | RDR2 DX12, 1 UE5 |
| 5 | XMX nesses jogos (consequência do 4) | Não | Os mesmos |

Release: **1.11.0**, porque a 1.10.0 está fechando com os itens atuais.

## 6. Perguntas

1. Aprova a abordagem: XMX Vulkan pelo provider MIT do dashdogy, e "force FG" só em jogos Streamline só-SR, sem sessão própria?
2. Pode a etapa de VRAM tocar no XMX DX12 como opção desligada por padrão? Ou só a medição por enquanto?
3. Fica para a 1.11.0, ou quer algum destes já na 1.10.0?
