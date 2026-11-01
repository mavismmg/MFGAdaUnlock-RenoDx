# Modo XMX: a rota direta do XeFG como terceiro modo

Item D.1 do `docs/plan/plano-1.8.md`. Leitura de `realdody/dlssg-to-xefg-xmx` (build-3, commit `bcadaab`, 2026-11-01) e do nosso código na `feat/1.8`. Nada aqui foi executado nem medido: tudo sobre desempenho e compatibilidade é hipótese até o protótipo rodar.

## Como a rota direta funciona

1. A DLL substitui o `nvngx_dlssg.dll`, exportando as mesmas funções `NVSDK_NGX_D3D12_*`. Um carregador entrega essa DLL à NGX: hoje, o OptiScaler 10.
2. O Streamline, o plugin DLSS-G e o driver continuam donos da apresentação. **Não há proxy de swapchain nem XeLL.**
3. Em cada avaliação, o gerador copia backbuffer, profundidade, vetores e HUD-less e chama a API interna do XeFG (`CreateContext`, `Init`, `Execute`), gravando os quadros nas saídas que o DLSS-G apresenta.
4. Essa API não é exportada. Os endereços vêm de uma tabela por build (1.3.1 e 1.3.4) ou de uma varredura; as estruturas só foram verificadas nessas duas versões.
5. O limite de quadros e o modelo são trocados escrevendo nas configurações internas da `libxess_fg.dll`, e o modelo 16 vem do porte do dashdogy. O README diz que não altera código, mas ainda altera memória do processo.

## Comparação

| | XeMFG atual (CUDA) | Modo XMX |
|---|---|---|
| Quem apresenta | proxy XeFG, nosso flip metering | Streamline, DLSS-G e driver NVIDIA |
| Latência | XeLL, ou Reflex como opção | Reflex nativo |
| Modelo | 16 | 16 |
| Execução da rede | CUDA nos Tensor Cores, com shaders no restante | shaders; sem rota CUDA |
| Entradas | Streamline, FSR, DLSS-SR | só as que o DLSS-G recebe |
| Vulkan | sim | não |
| RTX 20/30 | sim, com a API sintética | a verificar: o Streamline recusa DLSS-G nessas placas |
| Dependência de versão | tabelas do host, verificadas | tabela ou varredura da API interna |

## O que esperar

- **Compatibilidade:** é o principal ganho potencial. Sem proxy, somem as diferenças de apresentação entre o XeMFG e o DLSS-G. Isso pode ajudar jogos como o Dying Light 2, mas o crash dele ainda não tem causa conhecida.
- **Pacing e latência:** passam a ser os do DLSS-G, que já medimos como bons. É hipótese que o XeFG se comporte igual nesse caminho.
- **Desempenho:** provavelmente não é a vantagem. O modelo é o mesmo, e sem CUDA a rede pode ficar mais lenta nas NVIDIA. Isso reforça a decisão de manter o CUDA.
- **CUDA no modo XMX:** a rota intercepta os dispatches da própria biblioteca, então talvez funcione também aqui. Não há evidência; vale testar no protótipo.

## Como encaixa no addon

- **Seletor:** `Kind` ganha um terceiro valor e `[Backend] Selected` aceita `XMX`. O padrão não muda, e os valores atuais continuam válidos.
- **Base:** o modo parte do backend DLSS-G, que já tem seletor de multiplicador, Reflex e hooks do Streamline.
- **Ponto de entrada:** o `loadhook.hpp` já detecta o `nvngx_dlssg` ao carregar. Com o modo XMX, as entradas `NVSDK_NGX_D3D12_*` desse módulo passam a ser as nossas. Assim o usuário não precisa de OptiScaler.
- **Conflito a resolver:** o backend DLSS-G aplica correções ao módulo da NVIDIA (o `gate` de arquitetura e o midpoint). No modo XMX, elas ficam desligadas.
- **Reuso:** `vendor/port/` e `vendor/runtime_payload` já são o porte MIT do dashdogy.

## Licença

O projeto de referência é GPLv3 e o nosso é MIT. Não copiamos código nem traduzimos arquivos dele. Os endereços e layouts da API interna devem vir da nossa própria análise das duas DLLs, e o registro fica em `PROVENANCE.md`. Isso é cuidado de engenharia, não parecer jurídico.

## Riscos

- Cada nova `libxess_fg.dll` pode exigir nova análise. Versão desconhecida deve cair para o modo atual, com aviso.
- Uma atualização do `nvngx_dlssg` pela NGX pode mudar parâmetros que o substituto precisa responder.
- Jogos que validam o `nvngx_dlssg` podem recusar a substituição.

## Próximos passos

| # | Tarefa | Precisa de você? |
|---|---|---|
| D.2a | Análise própria da API interna nas versões 1.3.1 e 1.3.4 | não |
| D.2b | `native_xmx`: entradas NGX, captura, `Execute`, modo no INI, desligado por padrão | não |
| D.2c | Teste no Streamline Sample real, que já está na bateria | só não estar jogando, porque abre janela |
| D.2d | Testes em jogo: Cyberpunk 2077, Crimson Desert, medição contra CUDA e DLSS-G; depois, Dying Light 2 com o Cpt.Hund | sim |
| D.3 | Seletor no painel, só depois do D.2d | decisão |

## Análise da DLL (D.2a, em andamento)

`libxess_fg.dll` 1.3.1.78, SHA-256 `ec5e0c65…`, carimbo `0x69CB0F4D`. A análise usa só o RTTI e as exportações da própria DLL, com um leitor de PE próprio.

- **Sem API interna exportada:** nenhum nome `xefgD3D12*`. As funções internas precisam ser localizadas pelas tabelas de métodos.
- **`XefgContext`:** vtable na RVA `0xF45D48`, 23 métodos. `XefgContextInterface`, a base abstrata, fica em `0xF45C60`.
- **`XefgModel`:** vtable em `0xF47370`; `XefgModelUniversal` em `0xF47460`, 24 métodos cada.
- **Métodos identificados pelas strings que usam:** `XefgContext+0x20` (RVA `0x18BFC0`) é o `Init`, e `XefgContext+0x40` (RVA `0x18B210`) é o `Execute`. `XefgContext+0x08` (RVA `0x18A4F0`) escolhe e registra o modelo. O `XefgContext+0x10` (RVA `0x18BDD0`) carrega o modelo para o número de quadros pedido. `XefgModelUniversal+0x50` (RVA `0x19C030`) carrega um modelo pelo id.
- **Construtores:** o vtable do contexto é escrito em `0x188D3F`, `0x188FB5` e `0x1897D7`.
- **Configurações internas:** a função `0x1A51C0` confirma pela nossa leitura os campos `+0x4D` e `+0x64` (permitir e ativar a troca de modelo) e `+0x68` (id), além de `+0x4B` (usar XMX).
- **Fábrica:** `0x1853B0` chama o construtor `0x188D30` e cria o contexto sem o proxy de swapchain, que é criado à parte em `0x1FFDC0`, o `xefgSwapChainD3D12CreateContext` exportado.
- **`Init(contexto, parâmetros)`:** lê e copia `0x24` bytes. `+0x08` é o formato, validado; `+0x0C` são flags; `+0x10`, quando maior que 1, troca o modelo para multiquadro (provável limite de quadros). Se o multiquadro não for possível, retorna `-10`.
- **`Execute(contexto, lista de comandos, parâmetros)`:** antes do `Init`, retorna `-3`. Dois quadros de `0x100` bytes, em `+0x18` e `+0x118`, cada um com quatro recursos de `0x18` bytes seguidos de `0xA0` bytes de dados. As saídas ficam em `+0x230` (ponteiros) e `+0x240` (quantidade). `+0x248`, `+0x250`, `+0x260` e `+0x264` são opções, sem papel confirmado ainda.
- **Ainda falta:** a ordem dos quatro recursos e dos dados, confirmada só em execução, e repetir a análise na versão 1.3.4, que não temos aqui.

## Sonda em execução (2026-11-01)

`[DLSSG] XmxProbe=1` no Streamline Sample, nas versões 2.14.16 e 2.9.0. A geração da NVIDIA continuou funcionando nas duas.

- **Fábrica confirmada:** `0x1853B0` devolve um invólucro. A palavra `[0]` vale `842094660`, e a `[1]` aponta para um objeto cujo vtable é o do `XefgContext` (RVA `0xF45D48`).
- **Vários módulos:** o processo mapeia até três `nvngx_dlssg` (pasta do jogo, cópia em `files` e DriverStore). Cada um precisa de seu próprio hook.
- **Hook no `CreateFeature` da NVIDIA:** faz o módulo falhar com `PlatformError` (`0xBAD00002`). Só que isso vale para o módulo original; não sabemos o efeito quando o nosso código substitui a função.
- **Avaliação observada, 1280x720:** `Backbuffer` e `HUDLess` em formato 87; `Depth` em 44; `MVecs` em 34; `OutputInterpolated` e `OutputReal` em formato 87. Em 2x, `MultiFrameIndex=1` e `MultiFrameCount=1`; `Reset=1` no primeiro quadro.
- **Gerador, Streamline Sample a 1280x720, com Reflex limitando a 225 FPS:** 2x, 3x, 4x e 6x sem falha de `Init` ou `Execute`. O painel mostrou Engine 112, 56 e 37 contra True 225 em 2x, 4x e 6x, e a imagem ficou sã. Esse teto prova a cadência, não o custo da geração nem a qualidade dos quadros intermediários; isso fica para os jogos.
- **Modelo:** com `maxFrames=5`, o `Init` troca o 18 pelo nosso 16 também em 2x. A comparação entre 18 e 16 é feita na rota XeMFG, com `XeFGStock2xModel`.
- **Mudança de tamanho:** cria outro contexto sem destruir o anterior, que pode estar em uso; a perda é limitada e ainda não foi exercitada.

## Ajustes de qualidade e desempenho

O SDK público não tem preset de qualidade nem resolução interna. Só há `SetSceneChangeThreshold` e os genéricos `SetParameterP`/`GetParameterP`, cujos parâmetros ainda não conhecemos.

O que a função `0x1A51C0` e o método `0x18BDD0` mostram sobre os modelos:

| Modelo | Papel na 1.3.1 |
|---|---|
| 18 | 2x, padrão fora de hardware com XMX |
| 17 | 2x com XMX; acima de 2x é trocado pelo 15 |
| 15 | multiquadro com XMX; doa os pesos do nosso 16 |
| 16 | não existe na DLL; é o nosso porte para NVIDIA |

Sem suporte a XMX, o XeFG original recusa mais de 2x com o modelo 18.

- **Qualidade em 2x:** comparar o modelo 18 com o quadro `t=1/2` do modelo 16. A chave `[XeFG] XeFGStock2xModel=1`, desligada por padrão, liga a rota 2x nativa que já existia. Exige reiniciar o jogo, limita a 2x e fica fora do porte. Falta testar em jogo, inclusive para saber se melhora a imagem.
- **Qualidade em 3x a 6x: descartado.** O modelo 18 só gera o quadro do meio, e repeti-lo geraria quadros nas posições erradas. O código já proíbe isso em `backend.cpp`. Um quadro por fluxo, que citei antes, não existe nessa família de modelos sem XMX.
- **Desempenho:** o modelo 17 também não é um candidato mais leve, porque é o 2x para XMX. Sem ajuste público, só resta procurar a resolução interna dentro do modelo. É o item mais incerto.

## Teste de campo: Crimson Desert, 2026-11-01

RTX 4070 SUPER, DLSS Qualidade, uma captura PresentMon de 30 s por caso, mesmo trajeto repetido à mão. O Cyberpunk 2077 foi pulado por decisão do usuário, então nada aqui foi visto em um segundo jogo. FPS base e exibido do XMX e do DLSS-G vêm da estimativa do PresentMon, e a latência pode não ser medida igual entre as rotas.

| Resolução | Caso | Exibido | Base | Latência média | p99 entre quadros | Intervalos grandes |
|---|---|---|---|---|---|---|
| 1440p | `off` | 44 | 44 | 45 ms | | 0 |
| 1440p | `xefg2x-18` | 78 | 39 | 47 ms | 17,8 ms | 3 |
| 1440p | `xefg2x-16` | 81 | 41 | 54 ms | 14,5 ms | 1 |
| 1440p | `xmx2x` | 76 | 38 | 44 ms | 17,4 ms | 3 |
| 1440p | `dlssg4x` | 136 | 34 | 43 ms | 10,3 ms | 8 |
| 1440p | `xefg4x` | 141 | 35 | 64 ms | 9,0 ms | 4 |
| 1440p | `xmx4x` | 139 | 35 | 42 ms | 10,5 ms | 6 |
| 1440p | `xefg6x` | 175 | 29 | 75 ms | 8,1 ms | 4 |
| 1440p | `xmx6x` | 189 | 32 | 42 ms | 7,8 ms | 18 |
| 4K DLDSR | `off` | 23 | 23 | 91 ms | 48,3 ms | 0 |
| 4K DLDSR | `dlssg4x` | 73 | 18 | 80 ms | 17,8 ms | 8 |
| 4K DLDSR | `xefg4x` | 71 | 18 | 125 ms | 18,2 ms | 2 |
| 4K DLDSR | `xmx4x` | 69 | 17 | 80 ms | 20,4 ms | 6 |

O que o usuário viu:

- **18 contra 16 em 2x:** o 18 ficou melhor no HUD, menos borrado, e levemente melhor em artefatos.
- **`xmx2x`:** imagem um pouco melhor que a do `xefg2x-16`, que usa o mesmo modelo. Causa desconhecida.
- **`dlssg4x`:** HUD muito bom, muito artefato e flicker neste jogo.
- **Blur em movimento:** percebido no XMX, no DLSS-G e no XeMFG, então não é do XMX. Causa desconhecida.
- **`xefg6x`:** HUD e artefatos pioram bastante em relação ao 4x.
- **`xmx6x`:** imagem parecida com a do `xefg6x`, input muito melhor.
- **Robustez do XMX:** Alt+Tab e troca de resolução passaram. O log confirma o novo contexto em 2560x1440.
- **Bug achado no teste:** `XeFGStock2xModel` pedia 5 quadros e o `Init` falhava com -4. Corrigido em c4d3894.

Limitações: a captura do `xefg2x-18` rodou em `Hardware Composed: Independent Flip`, e as outras em `Hardware: Independent Flip`, por isso esse par pede repetição. Faltam a imagem do `xmx4x`, sem comentário do usuário, e a comparação de custo em GPU, porque o valor do 18 não é confiável.

## Hook e fallback (2026-11-01)

Correção: a sonda não era inofensiva. Com o `Evaluate` do `nvngx_dlssg` hookado, repassar a chamada ao original deixa a imagem preta no Streamline Sample 2.14.16, com o painel ainda dizendo "Generating frames". Sem o hook, a mesma configuração mostra imagem normal. Não sabemos por que o módulo para de gerar. Por isso:

- `XmxProbe` foi removida.
- `generator::Preflight` confere `libxess_fg.dll` (build 1.3.1.78) e `libxell.dll` antes de instalar o hook. Se falhar, não há hook e o gerador da NVIDIA continua, com aviso no painel.
- Falha depois do hook repete o quadro real: sem geração, mas sem tela preta, e com aviso no painel. O teste usou uma `libxess_fg.dll` truncada, que passa no cabeçalho e não carrega.
- Fora do Direct3D 12 o hook não atua, e o painel diz que a NVIDIA está gerando.

## Multiplicador e Dynamic MFG no XMX (2026-11-01)

- **Seletor do jogo:** no menu do Streamline Sample 2.14.16, a troca de 2x para 4x com o programa rodando funcionou (Engine 61, True 246). Ainda não foi testada no menu de um jogo.
- **Dynamic pedido pelo jogo:** com `eDynamic`, o XMX não gerava nada (Engine 240, True 240) e o painel dizia "Generating frames". Agora o pedido vira `eOn` com o multiplicador fixo (Engine 120, True 240) e o painel avisa. Não sabemos se o Dynamic da NVIDIA funcionaria nesta rota; ele fica bloqueado.
- **`DynamicMFG=1` salvo:** fica sem efeito no modo XMX e volta a valer ao selecionar DLSS-G.
