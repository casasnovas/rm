FILENAME = rm_scaleo.pdf

all: $FILENAME

$FILENAME:
	latex -output-format pdf rm_scaleo
	latex -output-format pdf rm_scaleo

clean:
	rm -f *.aux *.log *.toc *.bbl *.dvi *.pdf *.blg
	rm -Rf man/ latex/ html/