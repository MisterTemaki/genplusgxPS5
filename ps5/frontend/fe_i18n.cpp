// Genesis Plus GX PS5 frontend: the interface's languages (fe_i18n.h).
// SPDX-License-Identifier: MIT

#include "fe_i18n.h"

#include "fe_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace fe
{
namespace
{
// English, Spanish, Portuguese (Brazil), French, German. A text missing here is shown in English.
struct Entry
{
	const char* en;
	const char* es;
	const char* pt;
	const char* fr;
	const char* de;
};

const Entry kTable[] = {
	// ---- the settings
	{"On", "Sí", "Sim", "Oui", "Ein"},
	{"Off", "No", "Não", "Non", "Aus"},
	{"VIDEO", "VÍDEO", "VÍDEO", "VIDÉO", "VIDEO"},
	{"Shader", "Shader", "Shader", "Shader", "Shader"},
	{"Shader (GPU)", "Shader (GPU)", "Shader (GPU)", "Shader (GPU)", "Shader (GPU)"},
	{"Screen size", "Tamaño de la imagen", "Tamanho da imagem", "Taille de l'image", "Bildgröße"},
	{"Aspect ratio", "Relación de aspecto", "Proporção da tela", "Format d'image", "Seitenverhältnis"},
	{"NTSC filter (Blargg)", "Filtro NTSC (Blargg)", "Filtro NTSC (Blargg)", "Filtre NTSC (Blargg)", "NTSC-Filter (Blargg)"},
	{"Show the borders (overscan)", "Mostrar los bordes (overscan)", "Mostrar as bordas (overscan)",
		"Afficher les bords (overscan)", "Ränder zeigen (Overscan)"},
	{"Smooth picture", "Imagen suavizada", "Imagem suavizada", "Image lissée", "Geglättetes Bild"},
	{"(shader)", "(shader)", "(shader)", "(shader)", "(Shader)"},
	{"Scanlines (CRT effect)", "Scanlines (efecto CRT)", "Scanlines (efeito CRT)", "Lignes de balayage (effet CRT)",
		"Scanlines (Röhreneffekt)"},
	{"Game Gear LCD ghosting", "Estela LCD de Game Gear", "Rastro do LCD do Game Gear", "Rémanence LCD Game Gear",
		"Game-Gear-LCD-Nachzieheffekt"},
	{"Show FPS", "Mostrar FPS", "Mostrar FPS", "Afficher les FPS", "FPS anzeigen"},
	{"Renderer", "Renderizador", "Renderizador", "Rendu", "Renderer"},
	{"GPU", "GPU", "GPU", "GPU", "GPU"},
	{"CPU", "CPU", "CPU", "CPU", "CPU"},
	{"%s (after a restart)", "%s (al reiniciar)", "%s (ao reiniciar)", "%s (au redémarrage)", "%s (nach Neustart)"},
	{"AUDIO", "AUDIO", "ÁUDIO", "AUDIO", "AUDIO"},
	{"Sound", "Sonido", "Som", "Son", "Ton"},
	{"Volume", "Volumen", "Volume", "Volume", "Lautstärke"},
	{"FM sound chip", "Chip de sonido FM", "Chip de som FM", "Puce son FM", "FM-Soundchip"},
	{"Low-pass filter", "Filtro de paso bajo", "Filtro passa-baixa", "Filtre passe-bas", "Tiefpassfilter"},
	{"EMULATION", "EMULACIÓN", "EMULAÇÃO", "ÉMULATION", "EMULATION"},
	{"Fast forward speed (hold R2)", "Velocidad de avance rápido (mantén R2)", "Velocidade do avanço rápido (segure R2)",
		"Vitesse d'avance rapide (maintenir R2)", "Schnellvorlauf-Tempo (R2 halten)"},
	{"Unlimited", "Sin límite", "Sem limite", "Illimitée", "Unbegrenzt"},
	{"Rewind (hold L2 + R2)", "Rebobinar (mantén L2 + R2)", "Voltar no tempo (segure L2 + R2)",
		"Retour arrière (maintenir L2 + R2)", "Zurückspulen (L2 + R2 halten)"},
	{"Console region", "Región de la consola", "Região do console", "Région de la console", "Konsolenregion"},
	{"Remove the sprite limit", "Quitar el límite de sprites", "Remover o limite de sprites",
		"Supprimer la limite de sprites", "Sprite-Limit aufheben"},
	{"CD add-on (MD+ / MSU-MD)", "Accesorio de CD (MD+ / MSU-MD)", "Acessório de CD (MD+ / MSU-MD)",
		"Extension CD (MD+ / MSU-MD)", "CD-Erweiterung (MD+ / MSU-MD)"},
	{"CONTROLS (Mega Drive pad button  ->  PS5 button)", "CONTROLES (botón del mando Mega Drive  ->  botón PS5)",
		"CONTROLES (botão do controle Mega Drive  ->  botão do PS5)",
		"COMMANDES (bouton de la manette Mega Drive  ->  bouton PS5)",
		"STEUERUNG (Mega-Drive-Taste  ->  PS5-Taste)"},
	{"Mega Drive / Sega CD pad", "Mando de Mega Drive / Sega CD", "Controle do Mega Drive / Sega CD",
		"Manette Mega Drive / Sega CD", "Mega-Drive-/Sega-CD-Controller"},
	{"4-player adapter", "Adaptador de 4 jugadores", "Adaptador para 4 jogadores", "Adaptateur 4 joueurs",
		"4-Spieler-Adapter"},
	{"Button %s", "Botón %s", "Botão %s", "Bouton %s", "Taste %s"},
	{"Default button layout", "Botones por defecto", "Botões padrão", "Boutons par défaut", "Standardbelegung"},
	{"LIBRARY", "BIBLIOTECA", "BIBLIOTECA", "BIBLIOTHÈQUE", "BIBLIOTHEK"},
	{"Download covers", "Descargar portadas", "Baixar capas", "Télécharger les jaquettes", "Cover herunterladen"},
	{"SYSTEM", "SISTEMA", "SISTEMA", "SYSTÈME", "SYSTEM"},
	{"Language", "Idioma", "Idioma", "Langue", "Sprache"},
	{"Performance log", "Registro de rendimiento", "Registro de desempenho", "Journal de performances", "Leistungsprotokoll"},
	{"Debug logs", "Registros de depuración", "Registros de depuração", "Journaux de débogage", "Debug-Protokolle"},
	{"Updates (this is %s)", "Actualizaciones (esta es la %s)", "Atualizações (esta é a %s)",
		"Mises à jour (version actuelle %s)", "Updates (diese ist %s)"},
	{"Needs the helper", "Necesita el helper", "Precisa do helper", "Nécessite le helper", "Braucht den Helper"},
	{"Checking...", "Comprobando...", "Verificando...", "Vérification...", "Prüfe..."},
	{"Up to date", "Al día", "Atualizado", "À jour", "Aktuell"},
	{"Install %s (%.1f MB)", "Instalar %s (%.1f MB)", "Instalar %s (%.1f MB)", "Installer %s (%.1f Mo)",
		"%s installieren (%.1f MB)"},
	{"Downloading %d%%", "Descargando %d%%", "Baixando %d%%", "Téléchargement %d%%", "Lade herunter %d%%"},
	{"Installing %d/%d", "Instalando %d/%d", "Instalando %d/%d", "Installation %d/%d", "Installiere %d/%d"},
	{"Installed: restart", "Instalada: reinicia", "Instalada: reinicie", "Installée : redémarrez",
		"Installiert: neu starten"},
	{"Failed: try again", "Falló: inténtalo de nuevo", "Falhou: tente de novo", "Échec : réessayez",
		"Fehlgeschlagen: erneut versuchen"},
	{"Check now", "Comprobar ahora", "Verificar agora", "Vérifier maintenant", "Jetzt prüfen"},
	{"Updates are downloaded by the helper: send the payload, then open Settings again.",
		"El helper descarga las actualizaciones: envía el payload y vuelve a abrir Ajustes.",
		"O helper baixa as atualizações: envie o payload e abra as Configurações de novo.",
		"Le helper télécharge les mises à jour : envoyez le payload, puis rouvrez les Paramètres.",
		"Updates lädt der Helper: Payload senden, dann die Einstellungen erneut öffnen."},
	{"Back", "Volver", "Voltar", "Retour", "Zurück"},
	{"Settings", "Ajustes", "Configurações", "Paramètres", "Einstellungen"},
	{"Settings: %s", "Ajustes: %s", "Configurações: %s", "Paramètres : %s", "Einstellungen: %s"},
	{"Change", "Cambiar", "Mudar", "Changer", "Ändern"},
	{"Adjust", "Ajustar", "Ajustar", "Régler", "Anpassen"},
	// ---- the settings' choices
	{"Fit to screen", "Ajustar a la pantalla", "Ajustar à tela", "Adapter à l'écran", "An Bildschirm anpassen"},
	{"Integer scale", "Escala entera", "Escala inteira", "Échelle entière", "Ganzzahlige Skalierung"},
	{"Stretch to 16:9", "Estirar a 16:9", "Esticar para 16:9", "Étirer en 16:9", "Auf 16:9 strecken"},
	{"TV (the core's)", "TV (la del núcleo)", "TV (a do núcleo)", "TV (celui du cœur)", "TV (vom Kern)"},
	{"Square pixels", "Píxeles cuadrados", "Pixels quadrados", "Pixels carrés", "Quadratische Pixel"},
	{"Composite", "Compuesto", "Composto", "Composite", "Composite"},
	{"Monochrome", "Monocromo", "Monocromático", "Monochrome", "Monochrom"},
	{"Top and bottom", "Arriba y abajo", "Em cima e embaixo", "Haut et bas", "Oben und unten"},
	{"Left and right", "Izquierda y derecha", "Esquerda e direita", "Gauche et droite", "Links und rechts"},
	{"All", "Todos", "Todas", "Tous", "Alle"},
	{"Nuked YM2612 (accurate)", "Nuked YM2612 (preciso)", "Nuked YM2612 (preciso)", "Nuked YM2612 (précis)",
		"Nuked YM2612 (genau)"},
	{"Nuked YM3438 (accurate)", "Nuked YM3438 (preciso)", "Nuked YM3438 (preciso)", "Nuked YM3438 (précis)",
		"Nuked YM3438 (genau)"},
	{"Auto", "Auto", "Auto", "Auto", "Auto"},
	{"USA (NTSC)", "EE. UU. (NTSC)", "EUA (NTSC)", "États-Unis (NTSC)", "USA (NTSC)"},
	{"Europe (PAL)", "Europa (PAL)", "Europa (PAL)", "Europe (PAL)", "Europa (PAL)"},
	{"Japan (NTSC)", "Japón (NTSC)", "Japão (NTSC)", "Japon (NTSC)", "Japan (NTSC)"},
	{"None", "Ninguno", "Nenhum", "Aucun", "Keins"},
	{"Auto (the game's)", "Auto (el del juego)", "Auto (o do jogo)", "Auto (celle du jeu)", "Auto (vom Spiel)"},
	{"3 buttons", "3 botones", "3 botões", "3 boutons", "3 Tasten"},
	{"6 buttons", "6 botones", "6 botões", "6 boutons", "6 Tasten"},
	{"Touchpad", "Panel táctil", "Touchpad", "Pavé tactile", "Touchpad"},
	{"Cross", "Cruz", "X", "Croix", "Kreuz"},
	{"Circle", "Círculo", "Bola", "Rond", "Kreis"},
	{"Square", "Cuadrado", "Quadrado", "Carré", "Quadrat"},
	{"Triangle", "Triángulo", "Triângulo", "Triangle", "Dreieck"},
	{"C  (Master System 2)", "C  (Master System 2)", "C  (Master System 2)", "C  (Master System 2)",
		"C  (Master System 2)"},
	{"Start  (Pause)", "Start  (Pausa)", "Start  (Pausa)", "Start  (Pause)", "Start  (Pause)"},
	{"B  (Master System 1)", "B  (Master System 1)", "B  (Master System 1)", "B  (Master System 1)",
		"B  (Master System 1)"},
	// ---- the pause menu
	{"Resume", "Continuar", "Continuar", "Reprendre", "Fortsetzen"},
	{"Save state (slot %d)", "Guardar estado (ranura %d)", "Salvar estado (slot %d)", "Sauvegarder l'état (emplacement %d)",
		"Spielstand speichern (Platz %d)"},
	{"Load state (slot %d)", "Cargar estado (ranura %d)", "Carregar estado (slot %d)", "Charger l'état (emplacement %d)",
		"Spielstand laden (Platz %d)"},
	{"Load the previous state (slot %d)", "Cargar el estado anterior (ranura %d)", "Carregar o estado anterior (slot %d)",
		"Charger l'état précédent (emplacement %d)", "Vorherigen Spielstand laden (Platz %d)"},
	{"State slot", "Ranura de estado", "Slot do estado", "Emplacement", "Spielstand-Platz"},
	{"%d (used)", "%d (usada)", "%d (usado)", "%d (utilisé)", "%d (belegt)"},
	{"%d (empty)", "%d (vacía)", "%d (vazio)", "%d (vide)", "%d (leer)"},
	{"Settings (this game's)", "Ajustes (de este juego)", "Configurações (deste jogo)", "Paramètres (de ce jeu)",
		"Einstellungen (dieses Spiels)"},
	{"Settings for this game", "Ajustes de este juego", "Configurações deste jogo", "Paramètres de ce jeu",
		"Einstellungen für dieses Spiel"},
	{"Its own", "Propios", "Próprias", "Propres", "Eigene"},
	{"The global ones", "Generales", "Gerais", "Généraux", "Allgemeine"},
	{"Reset", "Reiniciar", "Reiniciar", "Réinitialiser", "Reset"},
	{"Power cycle", "Apagar y encender", "Desligar e ligar", "Éteindre et rallumer", "Aus- und einschalten"},
	{"Back to the game list", "Volver a la lista de juegos", "Voltar à lista de jogos", "Retour à la liste des jeux",
		"Zurück zur Spieleliste"},
	{"Quit Genesis Plus GX PS5", "Salir de Genesis Plus GX PS5", "Sair do Genesis Plus GX PS5",
		"Quitter Genesis Plus GX PS5", "Genesis Plus GX PS5 beenden"},
	{"This game now keeps its own settings.", "Este juego ahora tiene sus propios ajustes.",
		"Este jogo agora tem configurações próprias.", "Ce jeu a maintenant ses propres paramètres.",
		"Dieses Spiel hat jetzt eigene Einstellungen."},
	{"This game uses the global settings again.", "Este juego vuelve a usar los ajustes generales.",
		"Este jogo voltou a usar as configurações gerais.", "Ce jeu utilise à nouveau les paramètres généraux.",
		"Dieses Spiel nutzt wieder die allgemeinen Einstellungen."},
	{"Could not save this game's settings.", "No se pudieron guardar los ajustes de este juego.",
		"Não foi possível salvar as configurações deste jogo.", "Impossible d'enregistrer les paramètres de ce jeu.",
		"Die Einstellungen dieses Spiels konnten nicht gespeichert werden."},
	{"State saved.", "Estado guardado.", "Estado salvo.", "État sauvegardé.", "Spielstand gespeichert."},
	{"Could not save the state.", "No se pudo guardar el estado.", "Não foi possível salvar o estado.",
		"Impossible de sauvegarder l'état.", "Spielstand konnte nicht gespeichert werden."},
	{"This state can't be loaded.", "Este estado no se puede cargar.", "Este estado não pode ser carregado.",
		"Cet état ne peut pas être chargé.", "Dieser Spielstand kann nicht geladen werden."},
	{"This slot is empty.", "Esta ranura está vacía.", "Este slot está vazio.", "Cet emplacement est vide.",
		"Dieser Platz ist leer."},
	{"This slot has no previous state.", "Esta ranura no tiene estado anterior.", "Este slot não tem estado anterior.",
		"Cet emplacement n'a pas d'état précédent.", "Dieser Platz hat keinen vorherigen Spielstand."},
	{"OK", "Aceptar", "OK", "OK", "OK"},
	{"Yes", "Sí", "Sim", "Oui", "Ja"},
	{"No", "No", "Não", "Non", "Nein"},
	// ---- the shelf
	{"Looking for games...", "Buscando juegos...", "Procurando jogos...", "Recherche des jeux...", "Suche Spiele..."},
	{"Looking for games... %zu found", "Buscando juegos... %zu encontrados", "Procurando jogos... %zu encontrados",
		"Recherche des jeux... %zu trouvés", "Suche Spiele... %zu gefunden"},
	{"Identifying games... %d%% (%zu of %zu)", "Identificando juegos... %d%% (%zu de %zu)",
		"Identificando jogos... %d%% (%zu de %zu)", "Identification des jeux... %d%% (%zu sur %zu)",
		"Erkenne Spiele... %d%% (%zu von %zu)"},
	{"Quit Genesis Plus GX PS5?", "¿Salir de Genesis Plus GX PS5?", "Sair do Genesis Plus GX PS5?",
		"Quitter Genesis Plus GX PS5 ?", "Genesis Plus GX PS5 beenden?"},
	{"No games found.", "No se encontraron juegos.", "Nenhum jogo encontrado.", "Aucun jeu trouvé.",
		"Keine Spiele gefunden."},
	{"Copy your games to their system's folder in", "Copia tus juegos a la carpeta de su sistema en",
		"Copie seus jogos para a pasta do sistema deles em", "Copiez vos jeux dans le dossier de leur système dans",
		"Kopiere deine Spiele in den Ordner ihres Systems in"},
	{"/data/genplus/roms   or   genplus/roms  on a USB drive", "/data/genplus/roms   o   genplus/roms  en un USB",
		"/data/genplus/roms   ou   genplus/roms  num pendrive", "/data/genplus/roms   ou   genplus/roms  sur une clé USB",
		"/data/genplus/roms   oder   genplus/roms  auf einem USB-Stick"},
	{"GameGear  .gg     SG1000  .sg     (cartridges also inside .zip)",
		"GameGear  .gg     SG1000  .sg     (cartuchos también en .zip)",
		"GameGear  .gg     SG1000  .sg     (cartuchos também em .zip)",
		"GameGear  .gg     SG1000  .sg     (cartouches aussi en .zip)",
		"GameGear  .gg     SG1000  .sg     (Module auch in .zip)"},
	{"Sega CD needs its BIOS in /data/genplus/bios.  Then open Genesis Plus GX PS5 again.",
		"Sega CD necesita su BIOS en /data/genplus/bios.  Luego vuelve a abrir Genesis Plus GX PS5.",
		"O Sega CD precisa da BIOS em /data/genplus/bios.  Depois abra o Genesis Plus GX PS5 de novo.",
		"Le Sega CD a besoin de son BIOS dans /data/genplus/bios.  Puis rouvrez Genesis Plus GX PS5.",
		"Sega CD braucht sein BIOS in /data/genplus/bios.  Dann Genesis Plus GX PS5 neu öffnen."},
	{"Downloading covers... %d left", "Descargando portadas... faltan %d", "Baixando capas... faltam %d",
		"Téléchargement des jaquettes... %d restantes", "Lade Cover... noch %d"},
	{"Downloading covers in the background... %d left", "Descargando portadas en segundo plano... faltan %d",
		"Baixando capas em segundo plano... faltam %d", "Téléchargement des jaquettes en arrière-plan... %d restantes",
		"Lade Cover im Hintergrund... noch %d"},
	{"Offline: covers next time", "Sin conexión: portadas la próxima vez", "Sem conexão: capas na próxima vez",
		"Hors ligne : jaquettes la prochaine fois", "Offline: Cover beim nächsten Mal"},
	{"no cover", "sin portada", "sem capa", "sans jaquette", "kein Cover"},
	{"Play", "Jugar", "Jogar", "Jouer", "Spielen"},
	{"Browse", "Explorar", "Navegar", "Parcourir", "Blättern"},
	{"System", "Sistema", "Sistema", "Système", "System"},
	{"Skip 10", "Saltar 10", "Pular 10", "Sauter 10", "10 weiter"},
	{"Get cover", "Obtener portada", "Baixar capa", "Récupérer la jaquette", "Cover holen"},
	{"Quit", "Salir", "Sair", "Quitter", "Beenden"},
	{"All games", "Todos los juegos", "Todos os jogos", "Tous les jeux", "Alle Spiele"},
	// ---- in a game
	{"Could not back up the save: the new one is kept beside it (.srm.new)",
		"No se pudo respaldar la partida: la nueva se guarda al lado (.srm.new)",
		"Não foi possível fazer backup do save: o novo fica ao lado (.srm.new)",
		"Impossible de sauvegarder la copie : la nouvelle est gardée à côté (.srm.new)",
		"Sicherung des Spielstands fehlgeschlagen: der neue liegt daneben (.srm.new)"},
	{"Could not back up the Sega CD saves: the new ones are kept beside them (.brm.new)",
		"No se pudieron respaldar las partidas de Sega CD: las nuevas se guardan al lado (.brm.new)",
		"Não foi possível fazer backup dos saves do Sega CD: os novos ficam ao lado (.brm.new)",
		"Impossible de copier les sauvegardes Sega CD : les nouvelles sont gardées à côté (.brm.new)",
		"Sicherung der Sega-CD-Spielstände fehlgeschlagen: die neuen liegen daneben (.brm.new)"},
	{"This state is damaged or from another version: not loaded",
		"Este estado está dañado o es de otra versión: no se cargó",
		"Este estado está danificado ou é de outra versão: não carregado",
		"Cet état est endommagé ou d'une autre version : non chargé",
		"Dieser Spielstand ist beschädigt oder von einer anderen Version: nicht geladen"},
	{"The state of slot %d is empty", "El estado de la ranura %d está vacío", "O estado do slot %d está vazio",
		"L'état de l'emplacement %d est vide", "Der Spielstand auf Platz %d ist leer"},
	{"The previous state of slot %d is empty", "El estado anterior de la ranura %d está vacío",
		"O estado anterior do slot %d está vazio", "L'état précédent de l'emplacement %d est vide",
		"Der vorherige Spielstand auf Platz %d ist leer"},
	{"The state of slot %d loaded", "Estado de la ranura %d cargado", "Estado do slot %d carregado",
		"État de l'emplacement %d chargé", "Spielstand von Platz %d geladen"},
	{"The previous state of slot %d loaded", "Estado anterior de la ranura %d cargado",
		"Estado anterior do slot %d carregado", "État précédent de l'emplacement %d chargé",
		"Vorheriger Spielstand von Platz %d geladen"},
	{"The slot's state is kept as its previous state: load it from the pause menu",
		"El estado de la ranura se guardó como su estado anterior: cárgalo desde el menú de pausa",
		"O estado do slot foi guardado como estado anterior: carregue-o pelo menu de pausa",
		"L'état de l'emplacement est gardé comme état précédent : chargez-le depuis le menu pause",
		"Der Spielstand des Platzes liegt als vorheriger Spielstand vor: über das Pausenmenü laden"},
	{"Could not load the state", "No se pudo cargar el estado", "Não foi possível carregar o estado",
		"Impossible de charger l'état", "Spielstand konnte nicht geladen werden"},
	{"State saved to slot %d", "Estado guardado en la ranura %d", "Estado salvo no slot %d",
		"État sauvegardé dans l'emplacement %d", "Spielstand auf Platz %d gespeichert"},
	{"Preparing the shader %s...", "Preparando el shader %s...", "Preparando o shader %s...",
		"Préparation du shader %s...", "Shader %s wird vorbereitet..."},
	{"Shader parameters", "Parámetros del shader", "Parâmetros do shader", "Paramètres du shader", "Shader-Parameter"},
	{"Shader parameters: %s", "Parámetros del shader: %s", "Parâmetros do shader: %s", "Paramètres du shader : %s",
		"Shader-Parameter: %s"},
	{"%d to adjust", "%d ajustables", "%d ajustáveis", "%d réglables", "%d einstellbar"},
	{"(shader Off)", "(shader desactivado)", "(shader desligado)", "(shader désactivé)", "(Shader aus)"},
	{"%s  (default %s, from %s to %s)", "%s  (por defecto %s, de %s a %s)", "%s  (padrão %s, de %s a %s)",
		"%s  (par défaut %s, de %s à %s)", "%s  (Standard %s, von %s bis %s)"},
	{"Default", "Por defecto", "Padrão", "Par défaut", "Standard"},
	{"Put them all back to the shader's own", "Volver a los valores del shader", "Voltar aos valores do shader",
		"Revenir aux valeurs du shader", "Alle auf die Werte des Shaders zurücksetzen"},
	{"Could not save the shader parameters", "No se pudieron guardar los parámetros del shader",
		"Não foi possível salvar os parâmetros do shader", "Impossible d'enregistrer les paramètres du shader",
		"Shader-Parameter konnten nicht gespeichert werden"},
	{"State slot %d (used)", "Ranura %d (usada)", "Slot %d (usado)", "Emplacement %d (utilisé)", "Platz %d (belegt)"},
	{"State slot %d (empty)", "Ranura %d (vacía)", "Slot %d (vazio)", "Emplacement %d (vide)", "Platz %d (leer)"},
	{"%s is too heavy here: choose a lighter shader (Settings, Shader)",
		"%s es demasiado pesado aquí: elige un shader más ligero (Ajustes, Shader)",
		"%s está pesado demais aqui: escolha um shader mais leve (Configurações, Shader)",
		"%s est trop lourd ici : choisissez un shader plus léger (Paramètres, Shader)",
		"%s ist hier zu aufwendig: einen leichteren Shader wählen (Einstellungen, Shader)"},
	{"Could not start the game", "No se pudo iniciar el juego", "Não foi possível iniciar o jogo",
		"Impossible de lancer le jeu", "Das Spiel konnte nicht gestartet werden"},
	{"The path of %s is too long (over 254 characters): move it to a shorter folder.",
		"La ruta de %s es demasiado larga (más de 254 caracteres): muévelo a una carpeta más corta.",
		"O caminho de %s é longo demais (mais de 254 caracteres): mova-o para uma pasta mais curta.",
		"Le chemin de %s est trop long (plus de 254 caractères) : déplacez-le dans un dossier plus court.",
		"Der Pfad von %s ist zu lang (über 254 Zeichen): in einen kürzeren Ordner verschieben."},
	{"There is no game Genesis Plus GX PS5 knows inside %s.", "No hay ningún juego que Genesis Plus GX PS5 conozca en %s.",
		"Não há nenhum jogo que o Genesis Plus GX PS5 conheça em %s.",
		"Il n'y a aucun jeu connu de Genesis Plus GX PS5 dans %s.",
		"In %s ist kein Spiel, das Genesis Plus GX PS5 kennt."},
	{"Sega CD games need the BIOS: %s. Copy it to %s/ and try again.",
		"Los juegos de Sega CD necesitan la BIOS: %s. Cópiala en %s/ e inténtalo de nuevo.",
		"Os jogos de Sega CD precisam da BIOS: %s. Copie-a para %s/ e tente de novo.",
		"Les jeux Sega CD ont besoin du BIOS : %s. Copiez-le dans %s/ et réessayez.",
		"Sega-CD-Spiele brauchen das BIOS: %s. Nach %s/ kopieren und erneut versuchen."},
	{"Genesis Plus GX could not start %s. Sega CD games need the BIOS of their region in %s: bios_CD_U.bin (USA), bios_CD_E.bin (Europe), bios_CD_J.bin (Japan).",
		"Genesis Plus GX no pudo iniciar %s. Los juegos de Sega CD necesitan la BIOS de su región en %s: bios_CD_U.bin (EE. UU.), bios_CD_E.bin (Europa), bios_CD_J.bin (Japón).",
		"O Genesis Plus GX não conseguiu iniciar %s. Os jogos de Sega CD precisam da BIOS da região deles em %s: bios_CD_U.bin (EUA), bios_CD_E.bin (Europa), bios_CD_J.bin (Japão).",
		"Genesis Plus GX n'a pas pu lancer %s. Les jeux Sega CD ont besoin du BIOS de leur région dans %s : bios_CD_U.bin (États-Unis), bios_CD_E.bin (Europe), bios_CD_J.bin (Japon).",
		"Genesis Plus GX konnte %s nicht starten. Sega-CD-Spiele brauchen das BIOS ihrer Region in %s: bios_CD_U.bin (USA), bios_CD_E.bin (Europa), bios_CD_J.bin (Japan)."},
	{"Genesis Plus GX could not load %s.", "Genesis Plus GX no pudo cargar %s.", "O Genesis Plus GX não conseguiu carregar %s.",
		"Genesis Plus GX n'a pas pu charger %s.", "Genesis Plus GX konnte %s nicht laden."},
	{"Genesis Plus GX PS5 has no access to /data", "Genesis Plus GX PS5 no tiene acceso a /data",
		"O Genesis Plus GX PS5 não tem acesso a /data", "Genesis Plus GX PS5 n'a pas accès à /data",
		"Genesis Plus GX PS5 hat keinen Zugriff auf /data"},
	{"Send GenesisPlusGXPS5-v%s.elf with the Payload Manager (it unlocks the app), then open Genesis Plus GX PS5 again.",
		"Envía GenesisPlusGXPS5-v%s.elf con el Payload Manager (desbloquea la app) y vuelve a abrir Genesis Plus GX PS5.",
		"Envie o GenesisPlusGXPS5-v%s.elf com o Payload Manager (ele libera o app) e abra o Genesis Plus GX PS5 de novo.",
		"Envoyez GenesisPlusGXPS5-v%s.elf avec le Payload Manager (il débloque l'app), puis rouvrez Genesis Plus GX PS5.",
		"GenesisPlusGXPS5-v%s.elf mit dem Payload Manager senden (er entsperrt die App), dann Genesis Plus GX PS5 neu öffnen."},
	{"Genesis Plus GX PS5: could not start %s", "Genesis Plus GX PS5: no se pudo iniciar %s",
		"Genesis Plus GX PS5: não foi possível iniciar %s", "Genesis Plus GX PS5 : impossible de lancer %s",
		"Genesis Plus GX PS5: %s konnte nicht gestartet werden"},
	{"Genesis Plus GX PS5: the game could not be started again. %s",
		"Genesis Plus GX PS5: el juego no se pudo volver a iniciar. %s",
		"Genesis Plus GX PS5: não foi possível reiniciar o jogo. %s",
		"Genesis Plus GX PS5 : le jeu n'a pas pu être relancé. %s",
		"Genesis Plus GX PS5: das Spiel konnte nicht neu gestartet werden. %s"},
	// ---- borders (2.1, Snes9x PS5's translations)
	{"Border", "Marco", "Moldura", "Bordure", "Rahmen"},
	{"Default frame", "Marco predeterminado", "Moldura padrão", "Cadre par défaut", "Standardrahmen"},
	{"Download all borders", "Descargar todos los marcos", "Baixar todas as molduras", "Télécharger toutes les bordures",
		"Alle Rahmen herunterladen"},
	{"%d left", "faltan %d", "faltam %d", "%d restantes", "noch %d"},
	{"%d queued", "%d en cola", "%d na fila", "%d en attente", "%d eingereiht"},
	{"Nothing to fetch", "Nada que descargar", "Nada para baixar", "Rien à télécharger", "Nichts zu laden"},
	{"Waiting for the helper", "Esperando al helper", "Aguardando o helper", "En attente du helper", "Warte auf den Helper"},
	{"%d fetched", "%d descargados", "%d baixadas", "%d téléchargées", "%d geladen"},
};

const char* const kLanguageNames[] = {"English", "Español", "Português (Brasil)", "Français", "Deutsch"};

const std::unordered_map<std::string, const Entry*>& Index()
{
	static const std::unordered_map<std::string, const Entry*> index = [] {
		std::unordered_map<std::string, const Entry*> m;
		for (const Entry& e : kTable)
			m.emplace(e.en, &e);
		return m;
	}();
	return index;
}
} // namespace

const char* LanguageName(int lang)
{
	return lang >= 0 && lang < int(Language::Count) ? kLanguageNames[lang] : kLanguageNames[0];
}

const char* T(const char* english)
{
	const int lang = Config().language;
	if (lang <= 0 || lang >= int(Language::Count) || !english)
		return english;
	const auto& index = Index();
	const auto it = index.find(english);
	if (it == index.end())
	{
#ifndef __PROSPERO__
		// the host tests: every text shown without a translation, once (GENPLUS_HOST_I18N_MISSING=<file>)
		if (const char* path = getenv("GENPLUS_HOST_I18N_MISSING"))
		{
			static std::mutex m;
			static std::set<std::string> seen;
			std::lock_guard<std::mutex> lock(m);
			if (seen.insert(english).second)
				if (FILE* f = fopen(path, "a"))
				{
					fprintf(f, "%s\n", english);
					fclose(f);
				}
		}
#endif
		return english;
	}
	const Entry& e = *it->second;
	const char* t = lang == 1 ? e.es : lang == 2 ? e.pt : lang == 3 ? e.fr : e.de;
	return t && *t ? t : english;
}
} // namespace fe
