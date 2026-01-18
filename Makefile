TARGET = gneomutt
CC = clang
# On passe en -O3 pour une optimisation maximale
CFLAGS = -Wall -Wextra -O3

# Séparation des drapeaux pour plus de clarté
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 vte-2.91)
GTK_LIBS   = $(shell pkg-config --libs gtk+-3.0 vte-2.91)

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

# 2. Règle de compilation principale (inclut désormais RES_SRC)
$(TARGET): $(SRC) $(RES_SRC)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(SRC) $(RES_SRC) -o $(TARGET) $(GTK_LIBS)

# Vérifie si tous les outils nécessaires sont installés sur le système
check-deps:
	@echo "Vérification des dépendances système..."
	@$(foreach bin,$(DEPENDENCIES),\
		which $(bin) > /dev/null 2>&1 || (echo "ERREUR: '$(bin)' n'est pas installé."; exit 1);)
	@echo "Toutes les dépendances sont présentes."

# Règle Debug : compile avec les symboles de débogage et sans optimisation
debug: clean
	$(CC) $(CFLAGS) -g -Og $(SRC) -o $(TARGET) $(GTK_FLAGS)
	@echo "Mode Debug activé. Utilisez 'gdb ./$(TARGET)' pour déboguer."

# Règle Test : vérifie si les fichiers nécessaires sont présents avant de lancer
test: all
@echo "--- Vérification des dépendances ---"
	@# 1. Vérification des outils externes
	@$(foreach bin,$(DEPENDENCIES),\
		command -v $(bin) >/dev/null 2>&1 || { echo "ERREUR: L'outil '$(bin)' est introuvable. Installez-le avec pacman."; exit 1; })
			@echo "Vérification des dépendances..."
	@# 2. Vérification des fichiers sources critiques (pour le développement)
	@test -f $(RES_XML) || (echo "ERREUR: Fichier $(RES_XML) manquant"; exit 1)
	
	@echo "--- Configuration OK. Lancement de $(TARGET) ---"
	./$(TARGET)

# Règle Help : affiche l'aide du Makefile
help:
	@echo "Usage: make [RÈGLE]"
	@echo ""
	@echo "Règles disponibles :"
	@echo "  all       : Compile l'exécutable standard (optimisé)"
	@echo "  check-deps: Vérification des dépendances"
	@echo "  debug     : Compile pour le débogage (gdb/valgrind)"
	@echo "  test      : Vérifie l'environnement et lance l'app"
	@echo "  run       : Compile et lance l'application"
	@echo "  install   : Installe le binaire dans $(BINDIR) (besoin de sudo)"
	@echo "  uninstall : Supprime le binaire du système"
	@echo "  clean     : Supprime le binaire local"
	@echo "  help      : Affiche ce message"

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
