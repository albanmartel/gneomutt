TARGET = gneomutt
TOOLS = eml_to_html eml_to_txt eml_to_std
CC = clang
# On passe en -O3 pour une optimisation maximale
CFLAGS = -Wall -Wextra -O3

# 1. Détection dynamique des paquets
GTK_PKG = gtk+-3.0
VTE_PKG = $(shell pkg-config --list-all | grep vte | cut -d' ' -f1 | head -n 1)
# Pour WebKitGTK sous GTK3, c'est généralement webkit2gtk-4.0 ou 4.1
WEBKIT_PKG = $(shell pkg-config --list-all | grep webkit2gtk | cut -d' ' -f1 | head -n 1)
GMIME_PKG = gmime-3.0

# Liste consolidée
PKGS = $(GTK_PKG) $(VTE_PKG) $(WEBKIT_PKG)

# Séparation des drapeaux récupérés dynamiquement par PKGS
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LIBS += $(shell pkg-config --libs $(PKGS))

# Drapeaux spécifiques et légers pour les deux outils GMime
TOOL_CFLAGS = -Wall -Wextra -O3 $(shell pkg-config --cflags $(GMIME_PKG))
TOOL_LIBS   = $(shell pkg-config --libs $(GMIME_PKG)) -lgumbo

# Drapeaux pour l'outil eml_to_html (GMime + Gumbo)
HTML_CFLAGS = -Wall -Wextra -O3 $(shell pkg-config --cflags $(GMIME_PKG))
HTML_LIBS = $(shell pkg-config --libs $(GMIME_PKG)) -lgumbo

# 2. Récupération des drapeaux via pkg-config
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
APPDIR = $(PREFIX)/share/applications
SYSTEM_ICONDIR = $(PREFIX)/share/icons/hicolor

# Séparation des drapeaux pour plus de clarté
CFLAGS += $(shell pkg-config --cflags gmime-3.0 gio-2.0 gio-unix-2.0 glib-2.0)
LIBS   += $(shell pkg-config --libs gmime-3.0 gio-2.0 gio-unix-2.0 glib-2.0)

# 3. Ajout de -rdynamic pour lier les signaux du XML (GtkBuilder)
LDFLAGS = $(LIBS) -rdynamic

SRC = src/gneomutt.c
DATADIR = data
RES_XML = $(DATADIR)/resources.xml
RES_SRC = resources.c
# Fichiers générés par glib-compile-resources
RES_OBJ = $(RES_SRC:.c=.o)

# Liste des outils externes nécessaires au runtime
DEPENDENCIES = neomutt mbsync notmuch msmtp w3m

# --- RÈGLES ---

all: $(TARGET) $(TOOLS)

# Règle pour générer le fichier C à partir du XML
$(RES_SRC): $(RES_XML) $(DATADIR)/interface.ui $(DATADIR)/icons/scalable/gneomutt.svg
	glib-compile-resources $(RES_XML) --target=$(RES_SRC) --sourcedir=$(DATADIR) --generate-source

# Règle de compilation principale
$(TARGET): $(SRC) $(RES_SRC)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(SRC) $(RES_SRC) -o $(TARGET) $(LDFLAGS)

# Règle de compilation spécifique pour eml_to_std (Inchangée)
eml_to_std: src/eml_to_std.c
	$(CC) $(TOOL_CFLAGS) src/eml_to_std.c -o eml_to_std $(TOOL_LIBS)

# Règle de compilation spécifique pour eml_to_txt (Inchangée)
eml_to_txt: src/eml_to_txt.c
	$(CC) $(TOOL_CFLAGS) src/eml_to_txt.c -o eml_to_txt $(TOOL_LIBS)

# Règle de compilation spécifique pour eml_to_html
eml_to_html: src/eml_to_html.c
	$(CC) $(HTML_CFLAGS) src/eml_to_html.c -o eml_to_html $(HTML_LIBS)

# Vérifie si tous les outils nécessaires sont installés
check-deps:
	@echo "Vérification des dépendances système..."
	@$(foreach bin,$(DEPENDENCIES),\
		which $(bin) > /dev/null 2>&1 || (echo "ERREUR: '$(bin)' n'est pas installé."; exit 1);)
	@pkg-config --exists $(GMIME_PKG) || (echo "ERREUR: '$(GMIME_PKG)' est manquant. Installez gmime3 avec pacman."; exit 1);
	@echo "Toutes les dépendances sont présentes."

# Règle Debug corrigée pour l'ordre de génération
debug: clean
	# 1. On force la regénération du fichier ressources après le clean
	$(MAKE) $(RES_SRC)
	# 2. On compile le programme principal et les outils avec les flags de debug
	$(CC) $(CFLAGS) -g -O0 $(GTK_CFLAGS) $(SRC) $(RES_SRC) -o $(TARGET) $(LDFLAGS)
	# MODIFIÉ : On ajoute -lgumbo pour eml_to_html (en utilisant HTML_LIBS ou en l'écrivant en dur)
	$(CC) $(HTML_CFLAGS) -g -O0 src/eml_to_html.c -o eml_to_html $(HTML_LIBS)
	$(CC) $(TOOL_CFLAGS) -g -O0 src/eml_to_std.c -o eml_to_std $(TOOL_LIBS)
	$(CC) $(TOOL_CFLAGS) -g -O0 src/eml_to_txt.c -o eml_to_txt $(TOOL_LIBS)
	@echo "======================================================="
	@echo "       Mode Debug activé avec succès (-g -O0)          "
	@echo "======================================================="

# Règle Test : Correction des indentations (Tabulations) 
test: all
	@echo "--- Vérification des dépendances ---"
	@$(foreach bin,$(DEPENDENCIES),\
		command -v $(bin) >/dev/null 2>&1 || { echo "ERREUR: L'outil '$(bin)' est introuvable. Installez-le."; exit 1; })
	@echo "Vérification des fichiers sources..."
	@test -f $(RES_XML) || (echo "ERREUR: Fichier $(RES_XML) manquant"; exit 1)
	@echo "--- Configuration OK. Lancement de $(TARGET) ---"
	./$(TARGET)

# Règle Help
help:
	@echo "Usage: make [RÈGLE]"
	@echo ""
	@echo "Règles disponibles :"
	@echo "  all       : Compile l'exécutable standard (optimisé)"
	@echo "  check-deps: Vérification des dépendances"
	@echo "  debug     : Compile pour le débogage (gdb/valgrind)"
	@echo "  test      : Vérifie l'environnement et lance l'app"
	@echo "  run       : Compile et lance l'application"
	@echo "  install   : Installe le binaire, l'icône et le raccourci"
	@echo "  uninstall : Supprime l'application du système"
	@echo "  clean     : Supprime le binaire local"

install: all
	@echo "Vérification des droits d'administration..." 
# 1. Installation du binaire
	sudo install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	sudo install -Dm755 eml_to_html $(DESTDIR)$(BINDIR)/eml_to_html
	sudo install -Dm755 eml_to_txt $(DESTDIR)$(BINDIR)/eml_to_txt
	sudo install -Dm755 eml_to_std $(DESTDIR)$(BINDIR)/eml_to_std
	
# 2. Installation du raccourci de bureau
	sudo install -Dm644 $(DATADIR)/gneomutt.desktop $(DESTDIR)$(APPDIR)/$(TARGET).desktop
	
# 3. Installation de l'icône vectorielle (Scalable)
	sudo install -Dm644 $(DATADIR)/icons/scalable/gneomutt.svg $(DESTDIR)$(SYSTEM_ICONDIR)/scalable/apps/$(TARGET).svg
	
# 4. Boucle pour installer automatiquement toutes les tailles de PNG (16x16, 32x32, etc.)
	@$(foreach size,16x16 32x32 64x64 128x128 256x256,\
		sudo install -Dm644 $(DATADIR)/icons/hicolor/$(size)/apps/gneomutt.png $(DESTDIR)$(SYSTEM_ICONDIR)/$(size)/apps/$(TARGET).png;)

# 5. Mise à jour des caches système (très important sous Arch)
	-sudo gtk-update-icon-cache -ft $(DESTDIR)$(SYSTEM_ICONDIR) 2>/dev/null || true
	-sudo update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	@echo "Installation de $(TARGET), des icônes et du raccourci réussie !"

uninstall:
	@echo "Vérification des droits d'administration..."
# Supprime le binaire et le raccourci
	sudo rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	sudo rm -f $(DESTDIR)$(BINDIR)/eml_to_html
	sudo rm -f $(DESTDIR)$(BINDIR)/eml_to_txt
	sudo rm -f $(DESTDIR)$(BINDIR)/eml_to_std
	sudo rm -f $(DESTDIR)$(APPDIR)/$(TARGET).desktop
	
# Supprime l'icône SVG
	sudo rm -f $(DESTDIR)$(SYSTEM_ICONDIR)/scalable/apps/$(TARGET).svg
	
# Supprime toutes les icônes PNG
	@$(foreach size,16x16 32x32 64x64 128x128 256x256,\
		sudo rm -f $(DESTDIR)$(SYSTEM_ICONDIR)/$(size)/apps/$(TARGET).png;)
	
# Rafraîchit le système
	-sudo gtk-update-icon-cache -ft $(DESTDIR)$(SYSTEM_ICONDIR) 2>/dev/null || true
	-sudo update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	@echo "Désinstallation de $(TARGET) terminée proprement."

clean:
	rm -f $(TARGET) $(TOOLS) $(RES_SRC) $(RES_OBJ)

run: all
	./$(TARGET)

.PHONY: all clean run install uninstall debug test help
