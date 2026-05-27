TARGET = gneomutt
CC = clang
# On passe en -O3 pour une optimisation maximale
CFLAGS = -Wall -Wextra -O3

# 1. Détection dynamique des paquets
GTK_PKG = gtk+-3.0
VTE_PKG = $(shell pkg-config --list-all | grep vte | cut -d' ' -f1 | head -n 1)
# Pour WebKitGTK sous GTK3, c'est généralement webkit2gtk-4.0 ou 4.1
WEBKIT_PKG = $(shell pkg-config --list-all | grep webkit2gtk | cut -d' ' -f1 | head -n 1)

# Liste consolidée
PKGS = $(GTK_PKG) $(VTE_PKG) $(WEBKIT_PKG)

# 2. Récupération des drapeaux via pkg-config
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
APPDIR = $(PREFIX)/share/applications
SYSTEM_ICONDIR = $(PREFIX)/share/icons/hicolor

# Séparation des drapeaux pour plus de clarté
CFLAGS += $(shell pkg-config --cflags gtk+-3.0 vte-2.91 webkit2gtk-4.1)
LIBS += $(shell pkg-config --libs gtk+-3.0 vte-2.91 webkit2gtk-4.1)

# 3. Ajout de -rdynamic pour lier les signaux du XML (GtkBuilder)
LDFLAGS = $(LIBS) -rdynamic

SRC = src/gneomutt.c
DATADIR = data
RES_XML = $(DATADIR)/resources.xml
RES_SRC = resources.c
# Fichiers générés par glib-compile-resources
RES_OBJ = $(RES_SRC:.c=.o)

# Liste des outils externes nécessaires au runtime
DEPENDENCIES = neomutt mbsync notmuch msmtp

# --- RÈGLES ---

all: $(TARGET)

# 1. Règle pour générer le fichier C à partir du XML
$(RES_SRC): $(RES_XML) $(DATADIR)/interface.ui $(DATADIR)/icons/scalable/gneomutt.svg
	glib-compile-resources $(RES_XML) --target=$(RES_SRC) --sourcedir=$(DATADIR) --generate-source

# 2. Règle de compilation principale
$(TARGET): $(SRC) $(RES_SRC)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(SRC) $(RES_SRC) -o $(TARGET) $(LDFLAGS)

# Vérifie si tous les outils nécessaires sont installés
check-deps:
	@echo "Vérification des dépendances système..."
	@$(foreach bin,$(DEPENDENCIES),\
		which $(bin) > /dev/null 2>&1 || (echo "ERREUR: '$(bin)' n'est pas installé."; exit 1);)
	@echo "Toutes les dépendances sont présentes." [cite: 3]

# Règle Debug : Correction de $(GTK_FLAGS) en $(GTK_CFLAGS) $(LIBS) 
debug: clean
	$(CC) $(CFLAGS) -g -Og $(SRC) $(RES_SRC) -o $(TARGET) $(GTK_CFLAGS) $(LIBS)
	@echo "Mode Debug activé. Utilisez 'gdb ./$(TARGET)' pour déboguer." 

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
	rm -f $(TARGET) $(RES_SRC) $(RES_OBJ)

run: all
	./$(TARGET)

.PHONY: all clean run install uninstall debug test help
