TARGET = gneomutt
CC = clang
# On passe en -O3 pour une optimisation maximale
CFLAGS = -Wall -Wextra -O3

# Définition des dossiers d'installation (Manquants précédemment)
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

# Séparation des drapeaux pour plus de clarté
CFLAGS += $(shell pkg-config --cflags gtk+-3.0 vte-2.91 webkit2gtk-4.0)
LIBS += $(shell pkg-config --libs gtk+-3.0 vte-2.91 webkit2gtk-4.0)

SRC = gneomutt.c
RES_XML = resources.xml
RES_SRC = resources.c
# Fichiers générés par glib-compile-resources
RES_OBJ = $(RES_SRC:.c=.o)

# Liste des outils externes nécessaires au runtime
DEPENDENCIES = neomutt mbsync notmuch msmtp

# --- RÈGLES ---

all: $(TARGET)

# 1. Règle pour générer le fichier C à partir du XML
$(RES_SRC): $(RES_XML)
	glib-compile-resources $(RES_XML) --target=$(RES_SRC) --generate-source

# 2. Règle de compilation principale
$(TARGET): $(SRC) $(RES_SRC)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(SRC) $(RES_SRC) -o $(TARGET) $(LIBS)

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
	@echo "  install   : Installe le binaire dans $(BINDIR)"
	@echo "  uninstall : Supprime le binaire du système"
	@echo "  clean     : Supprime le binaire local"

install: all
	@echo "Vérification des droits d'administration..." 
	sudo install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Installation de $(TARGET) réussie dans $(BINDIR)." 

uninstall:
	@echo "Vérification des droits d'administration..."
	sudo rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Désinstallation de $(TARGET) terminée."

clean:
	rm -f $(TARGET) $(RES_SRC) $(RES_OBJ)

run: all
	./$(TARGET)

.PHONY: all clean run install uninstall debug test help