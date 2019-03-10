ORG := iptfs.org
BASE := $(shell sed -e '/^\#+RFC_NAME:/!d;s/\#+RFC_NAME: *\(.*\)/\1/' $(ORG))
VERSION := $(shell sed -e '/^\#+RFC_VERSION:/!d;s/\#+RFC_VERSION: *\([0-9]*\)/\1/' $(ORG))
VBASE := publish/$(BASE)-$(VERSION)

all: $(BASE).xml $(VBASE).txt $(VBASE).html $(VBASE).pdf

clean:
	rm -f ${BASE}.xml ${BASE}-*.{txt,html,pdf}

ox-rfc.el:
	curl -fLO 'https://raw.githubusercontent.com/choppsv1/org-rfc-export/master/ox-rfc.el'

$(BASE).xml: $(ORG) ox-rfc.el
	emacs -Q --batch --eval '(setq org-confirm-babel-evaluate nil)' -l ./ox-rfc.el $< -f ox-rfc-export-to-xml

publish/%-$(VERSION).txt: %.xml
	xml2rfc --text -o $@ $<

publish/%-$(VERSION).html: %.xml
	xml2rfc --html -o $@ $<

publish/%-$(VERSION).pdf: %.xml
	xml2rfc --pdf -o $@ $<

# ------------
# Verification
# ------------

idnits: $(VBASE).txt
	if [ ! -e idnits ]; then curl -fLO 'http://tools.ietf.org/tools/idnits/idnits'; chmod 755 idnits; fi
	./idnits --verbose $<
