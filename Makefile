BUILD_DIR := build
BLOG_BUILD_DIR := $(BUILD_DIR)/blog
BLOG_PUBLIC := blog/public
BLOG_GEN := blog/resources/_gen

.PHONY: all build dev serve clean

all: build

build: clean
	@mkdir -p $(BLOG_BUILD_DIR)
	@cd blog && hugo --baseURL /blog
	@cp -rf $(BLOG_PUBLIC)/* $(BLOG_BUILD_DIR)
	@rm -rf $(BLOG_BUILD_DIR)/assets $(BLOG_BUILD_DIR)/favicon.ico $(BLOG_BUILD_DIR)/404.html
	@mv -f $(BLOG_BUILD_DIR)/post-assets build/post-assets

	@# I'll skit these for now until i figured out a way to better integrate
	@# my custom code with the website, and some other improvements.
	@rm -rf $(BLOG_BUILD_DIR)/categories $(BLOG_BUILD_DIR)/tags
	@python3 fix_sitemap.py
	@mv -f build/blog/sitemap.xml build/sitemap.xml

	@cp -rf root/* $(BUILD_DIR)/

dev:
	@cd blog && hugo server --buildDrafts --disableFastRender

serve:
	@cd build && python3 -m http.server

clean:
	@rm -rf $(BUILD_DIR) $(BLOG_PUBLIC) $(BLOG_GEN)
