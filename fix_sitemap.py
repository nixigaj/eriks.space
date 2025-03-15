import xml.etree.ElementTree as Et
import re

def fix_sitemap(input_file):
	Et.register_namespace('', 'http://www.sitemaps.org/schemas/sitemap/0.9')
	Et.register_namespace('xhtml', 'http://www.w3.org/1999/xhtml')

	tree = Et.parse(input_file)
	root = tree.getroot()

	ns = {'ns': 'http://www.sitemaps.org/schemas/sitemap/0.9'}

	for url in root.findall('ns:url', ns):
		loc = url.find('ns:loc', ns)
		if loc is not None and any(p in loc.text for p in ['/blog/categories/', '/blog/tags/']):
			root.remove(url)

	xml_str = Et.tostring(root, encoding='utf-8', method='xml').decode()

	xml_str = re.sub(
		r'(<urlset.*?)(/?>)',
		r'\1 xmlns:xhtml="http://www.w3.org/1999/xhtml"\2',
		xml_str,
		count=1
	)

	final_xml = '<?xml version="1.0" encoding="utf-8" standalone="yes"?>\n' + xml_str

	with open(input_file, 'w', encoding='utf-8') as f:
		f.write(final_xml)

fix_sitemap('build/blog/sitemap.xml')
