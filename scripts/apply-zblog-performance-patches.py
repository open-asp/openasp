#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Apply the local Z-Blog page optimizations to an unpacked site tree."""

from pathlib import Path
import sys


SIDEBAR_STYLE = """

/* Egret ASP: restored sidebar spacing and card treatment. */
#divSidebar {
	margin-top: 30px;
}

#divSidebar .function {
	box-sizing: border-box;
	margin-bottom: 16px;
	overflow: hidden;
	background: #fff;
	border: 1px solid #e5e7eb;
	border-radius: 4px;
}

#divSidebar .function .function_t {
	box-sizing: border-box;
	height: auto;
	min-height: 38px;
	margin: 0;
	padding: 7px 12px;
	line-height: 24px;
	font-size: 1.05em;
	font-weight: 600;
	color: #374151;
	background: #f6f8fb;
	border: 0;
	border-bottom: 1px solid #e5e7eb;
}

#divSidebar .function .function_c {
	box-sizing: border-box;
	padding: 4px 10px 10px;
	color: #4b5563;
}

#divSidebar .function .function_c div {
	padding: 8px 2px 2px;
}

#divSidebar .function ul {
	box-sizing: border-box;
	width: 100%;
	padding: 2px 4px;
	border: 0;
}

#divSidebar .function li {
	padding: 7px 4px 7px 14px;
	background-position: 3px 15px;
}

#divSidebar #edtSearch {
	box-sizing: border-box;
	width: calc(100% - 58px);
	min-width: 0;
	height: 32px;
	padding: 4px 7px;
	border: 1px solid #d1d5db;
}

#divSidebar #btnPost {
	height: 32px;
	padding: 4px 9px;
}

@media (max-width: 760px) {
	#divSidebar {
		margin-top: 24px;
	}
}
"""

SAFE_FRONTEND_STYLE = """

/* Egret ASP: scoped sidebar and footer fixes. */
#divSidebar #divMisc {
	margin-bottom: 6px;
}

#divSidebar #divMisc .function_c {
	padding: 2px 10px 6px;
}

#divSidebar #divMisc ul {
	display: flex;
	flex-wrap: wrap;
	align-items: center;
	gap: 8px;
	padding: 4px;
}

#divSidebar #divMisc li {
	flex: 0 0 auto;
	width: auto;
	margin: 0;
	padding: 0;
	line-height: 0;
	background: none;
}

#divSidebar #divMisc a,
#divSidebar #divMisc img {
	display: block;
}

#divBottom {
	box-sizing: border-box;
	height: auto;
	min-height: 0;
	padding: 14px 0 12px;
}

#divBottom #BlogPowerBy,
#divBottom #BlogCopyRight {
	height: auto;
	min-height: 0;
	margin: 0;
	padding: 0;
	line-height: 1.5;
}

@media (max-width: 760px) {
	#divBottom {
		padding: 18px 12px 14px;
	}
}
"""

FOOTER_BACKGROUND_STYLE = """

/* Egret ASP: align footer background with the last sidebar card. */
#divSidebar #divMisc {
	margin-bottom: 8px;
}

#divPage {
	background: none;
	overflow-x: clip;
}

#divBottom {
	position: relative;
	z-index: 0;
	isolation: isolate;
}

#divBottom::before {
	position: absolute;
	z-index: -1;
	top: 0;
	bottom: 0;
	left: 50%;
	width: 100vw;
	content: "";
	pointer-events: none;
	background: url("default/bg-bottom.png") repeat-x 0 bottom;
	transform: translateX(-50%);
}
"""

ICON_STACK_STYLE = """

/* Egret ASP: stack miscellaneous icons one per row. */
#divSidebar #divMisc ul {
	flex-direction: column;
	flex-wrap: nowrap;
	align-items: flex-start;
	gap: 6px;
}
"""

ADMIN_SIDEBAR_STYLE = """

/* Egret ASP: preserve desktop admin navigation label width. */
@media (min-width: 761px) {
	#main .main_left #leftmenu span {
		box-sizing: border-box;
		width: 140px;
		padding: 0 40px 0 10px;
		overflow: hidden;
		white-space: nowrap;
		background-position: 108px 8px;
	}

	#main .main_left #leftmenu li.sub span {
		width: 140px;
		padding: 0 0 0 35px;
	}
}
"""


def replace_once(text: str, old: str, new: str, label: str) -> str:
    """Apply a version-sensitive edit and fail loudly if upstream text drifted."""
    if old not in text:
        raise RuntimeError(f"{label}: expected upstream text was not found")
    return text.replace(old, new, 1)


def patch_tags(root: Path) -> None:
    """Replace quadratic ReDim Preserve growth with one counted allocation."""
    target = root / "tags.asp"
    text = target.read_text(encoding="utf-8-sig")
    if "Dim i,j,tagCount" in text:
        return
    text = replace_once(
        text,
        """Dim i,j

For Each Tag in Tags""",
        """Dim i,j,tagCount

tagCount=0
For Each Tag in Tags
\tIf IsObject(Tag) Then
\t\tIf Tag.Count<>0 Then tagCount=tagCount+1
\tEnd If
Next

If tagCount>0 Then ReDim strTagCloud(tagCount-1)

For Each Tag in Tags""",
        str(target),
    )
    text = replace_once(text, "\t\t\tReDim Preserve strTagCloud(j+1)\n", "", str(target))
    text = replace_once(
        text,
        '\t\t\tstrTagCloud(j) = "<a href=""" & Tag.Url &""" title=""" & Tag.Count & """  class=""tag-name tag-name-size-"&i&""">" &Tag.name & "</a>"\n',
        '\t\t\tstrTagCloud(j) = "<a href=""" & Tag.Url &""" title=""" & Tag.Count & """  class=""tag-name tag-name-size-"&i&""">" &Tag.name & "</a>"\n\t\t\tj=j+1\n',
        str(target),
    )
    text = replace_once(text, "\tEnd If\n\tj=j+1\nNext", "\tEnd If\nNext", str(target))
    text = replace_once(
        text,
        'objArticle.Content="<div class=""tags-cloud"">"&Join(strTagCloud)&"</div>"',
        'If tagCount>0 Then\n\tobjArticle.Content="<div class=""tags-cloud"">"&Join(strTagCloud)&"</div>"\nElse\n\tobjArticle.Content="<div class=""tags-cloud""></div>"\nEnd If',
        str(target),
    )
    target.write_text(text, encoding="utf-8")


def patch_feed_cache(root: Path) -> None:
    """Rebuild the default RSS cache on demand after a disposable site restore."""
    target = root / "feed.asp"
    text = target.read_text(encoding="utf-8-sig")
    if "Egret ASP: rebuild a missing default RSS cache." in text:
        return
    text = replace_once(
        text,
        """Else
\tResponse.Write LoadFromFile(BlogPath & "zb_users\\cache\\rss.xml" ,"utf-8")
End If""",
        """Else
\t' Egret ASP: rebuild a missing default RSS cache.
\tDim strRssCache
\tstrRssCache=LoadFromFile(BlogPath & "zb_users\\cache\\rss.xml" ,"utf-8")
\tIf Len(strRssCache)=0 Then
\t\tCall ExportRSS()
\t\tstrRssCache=LoadFromFile(BlogPath & "zb_users\\cache\\rss.xml" ,"utf-8")
\tEnd If
\tResponse.Write strRssCache
End If""",
        str(target),
    )
    target.write_text(text, encoding="utf-8")


def patch_include_path_case(root: Path) -> None:
    """Use the writer's lowercase include path on case-sensitive hosts."""
    target = root / "zb_system/FUNCTION/c_system_base.asp"
    text = target.read_text(encoding="utf-8-sig")
    text = text.replace("zb_users\\INCLUDE", "zb_users\\include")
    target.write_text(text, encoding="utf-8")


def repair_sidebar(root: Path) -> None:
    """Discard invalid generated pages while preserving the last clean homepage."""
    cache_dir = root / "zb_users/cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    default_page = cache_dir / "default.asp"
    default_backup = cache_dir / "default.openasp-backup.asp"
    invalid_marker = "<#CACHE_INCLUDE_"
    default_invalid = (
        default_page.exists()
        and (
            default_page.stat().st_size == 0
            or invalid_marker in default_page.read_text(encoding="utf-8-sig")
        )
    )
    backup_valid = (
        default_backup.exists()
        and default_backup.stat().st_size > 0
        and invalid_marker not in default_backup.read_text(encoding="utf-8-sig")
    )
    if default_invalid:
        if backup_valid:
            default_page.write_bytes(default_backup.read_bytes())
        else:
            default_page.unlink()
    elif default_page.exists():
        default_backup.write_bytes(default_page.read_bytes())


def patch_sidebar_style(root: Path) -> None:
    """Install the scoped compatibility CSS idempotently and bump its cache key."""
    target = root / "zb_users/THEME/default/STYLE/default.css"
    text = target.read_text(encoding="utf-8-sig")
    broad_marker = "/* Egret ASP: modern frontend compatibility and layout fixes v2. */"
    if broad_marker in text:
        text = text.split(broad_marker, 1)[0].rstrip() + "\n"
    if "Egret ASP: restored sidebar spacing and card treatment." not in text:
        text = text.rstrip() + SIDEBAR_STYLE + "\n"
    if "Egret ASP: scoped sidebar and footer fixes." not in text:
        text = text.rstrip() + SAFE_FRONTEND_STYLE + "\n"
    if "Egret ASP: align footer background with the last sidebar card." not in text:
        text = text.rstrip() + FOOTER_BACKGROUND_STYLE + "\n"
    if "Egret ASP: stack miscellaneous icons one per row." not in text:
        text = text.rstrip() + ICON_STACK_STYLE + "\n"
    text = text.replace(
        "/* Egret ASP: align footer background with the last sidebar card. */\n"
        "#divSidebar #divMisc {\n\tmargin-bottom: 0;\n}",
        "/* Egret ASP: align footer background with the last sidebar card. */\n"
        "#divSidebar #divMisc {\n\tmargin-bottom: 8px;\n}",
    )
    text = text.replace(
        "#divPage {\n\tbackground: none;\n}\n\n#divBottom {",
        "#divPage {\n\tbackground: none;\n\toverflow-x: clip;\n}\n\n#divBottom {",
    )
    target.write_text(text, encoding="utf-8")

    source = root / "zb_users/THEME/default/SOURCE/style.css.asp"
    source_text = source.read_text(encoding="utf-8-sig")
    source_text = source_text.replace(
        '& ".css" & """);")',
        '& ".css?v=egret-safe-ui-5" & """);")',
    )
    source_text = source_text.replace(
        "?v=egret-sidebar-card-1", "?v=egret-safe-ui-5"
    ).replace("?v=egret-modern-3", "?v=egret-safe-ui-5").replace(
        "?v=egret-safe-ui-1", "?v=egret-safe-ui-5"
    ).replace("?v=egret-safe-ui-2", "?v=egret-safe-ui-5").replace(
        "?v=egret-safe-ui-3", "?v=egret-safe-ui-5"
    ).replace("?v=egret-safe-ui-4", "?v=egret-safe-ui-5")
    source.write_text(source_text, encoding="utf-8")


def patch_admin_sidebar_style(root: Path) -> None:
    """Keep legacy desktop menu dimensions under the global border-box reset."""
    stylesheet = root / "zb_system/CSS/admin2.css"
    text = stylesheet.read_text(encoding="utf-8-sig")
    text = text.replace(
        "img, iframe, video { max-width: 100%; }",
        "img, video { max-width: 100%; }",
    )
    marker = "Egret ASP: preserve desktop admin navigation label width."
    if marker in text:
        text = text.split(marker, 1)[0].rstrip()
        if text.endswith("/*"):
            text = text[:-2].rstrip()
        text += ADMIN_SIDEBAR_STYLE + "\n"
    else:
        text = text.rstrip() + ADMIN_SIDEBAR_STYLE + "\n"
    stylesheet.write_text(text, encoding="utf-8")

    header = root / "zb_system/ADMIN/admin_header.asp"
    header_text = header.read_text(encoding="utf-8-sig")
    header_text = header_text.replace(
        "ZB_SYSTEM/CSS/admin2.css?v=egret-admin-nav-1",
        "ZB_SYSTEM/CSS/admin2.css?v=egret-admin-ui-3",
    ).replace(
        "ZB_SYSTEM/CSS/admin2.css?v=egret-admin-nav-2",
        "ZB_SYSTEM/CSS/admin2.css?v=egret-admin-ui-3",
    ).replace(
        "ZB_SYSTEM/CSS/admin2.css",
        "ZB_SYSTEM/CSS/admin2.css?v=egret-admin-ui-3",
        1,
    )
    header_text = header_text.replace(
        "admin2.css?v=egret-admin-ui-3?v=egret-admin-ui-3",
        "admin2.css?v=egret-admin-ui-3",
    )
    header.write_text(header_text, encoding="utf-8")


def restore_safe_theme_markup(root: Path) -> None:
    """Undo browser-incompatible generated markup using configured site values."""
    option_text = (root / "zb_users/c_option.asp").read_text(encoding="utf-8-sig")

    def option(name: str, default: str) -> str:
        prefix = f'{name}="'
        for line in option_text.splitlines():
            if line.startswith(prefix) and line.endswith('"'):
                return line[len(prefix):-1]
        return default

    blog_title = option("ZC_BLOG_TITLE", "Z-Blog")
    blog_subtitle = option("ZC_BLOG_SUBTITLE", "Hello, world!")
    blog_theme = option("ZC_BLOG_THEME", "default")
    copyright_text = option(
        "ZC_BLOG_COPYRIGHT", "Copyright Your WebSite. Some Rights Reserved."
    )

    targets = [
        root / "zb_users/THEME/default/TEMPLATE/header.html",
        root / "zb_users/THEME/default/TEMPLATE/default.html",
        root / "zb_users/THEME/default/TEMPLATE/page.html",
        root / "zb_users/THEME/default/TEMPLATE/single.html",
        root / "zb_users/THEME/default/TEMPLATE/catalog.html",
        root / "zb_users/include/searchpanel.asp",
        root / "zb_users/include/favorite.asp",
        root / "zb_users/include/link.asp",
        root / "zb_users/include/misc.asp",
        root / "zb_users/cache/default.asp",
    ]
    for target in targets:
        if not target.exists():
            continue
        text = target.read_text(encoding="utf-8-sig")
        text = text.replace("<script defer src=", "<script src=")
        text = text.replace(
            "zb_users/theme/<#ZC_BLOG_THEME#>/style/<#ZC_BLOG_CSS#>.css"
            "?v=egret-modern-3",
            "zb_users/theme/<#ZC_BLOG_THEME#>/source/style.css.asp",
        )
        text = text.replace(
            f"zb_users/theme/{blog_theme}/style/default.css?v=egret-modern-3",
            f"zb_users/theme/{blog_theme}/source/style.css.asp",
        )
        text = text.replace(
            '<div id="divNavBar" role="navigation" aria-label="\u4e3b\u5bfc\u822a">',
            '<div id="divNavBar">',
        ).replace(
            '<div id="divMain" role="main">',
            '<div id="divMain">',
        ).replace(
            '<div id="divSidebar" role="complementary" aria-label="\u4fa7\u680f">',
            '<div id="divSidebar">',
        ).replace(
            '<div id="divBottom" role="contentinfo">',
            '<div id="divBottom">',
        )
        text = text.replace(
            'type="search" name="edtSearch" aria-label="\u641c\u7d22\u5173\u952e\u8bcd"',
            'type="text" name="edtSearch"',
        )
        text = text.replace(' rel="noopener noreferrer"', "")
        text = text.replace(' loading="lazy" decoding="async"', "")
        text = text.replace("https://www.zblogcn.com/", "http://www.zblogcn.com/")

        if target.name == "default.asp":
            text = text.replace(
                f'<h1 id="BlogTitle"><a href="{option("ZC_BLOG_HOST", "")}"></a></h1>',
                f'<h1 id="BlogTitle"><a href="{option("ZC_BLOG_HOST", "")}">'
                f"{blog_title}</a></h1>",
            )
            text = text.replace(
                '<h3 id="BlogSubTitle"></h3>',
                f'<h3 id="BlogSubTitle">{blog_subtitle}</h3>',
            )
            text = text.replace(
                '<h5 class="post-tags">: ',
                '<h5 class="post-tags">Tags: ',
            )
            text = text.replace(
                "\t\t:admin | :\u672a\u5206\u7c7b | :5 | :",
                "\t\t\u53d1\u5e03:admin | \u5206\u7c7b:\u672a\u5206\u7c7b | "
                "\u8bc4\u8bba:5 | \u6d4f\u89c8:",
            )
            text = text.replace(
                'cmd.asp?act=login">[]</a>',
                'cmd.asp?act=login">[\u7528\u6237\u767b\u5f55]</a>',
            ).replace(
                'cmd.asp?act=vrs">[]</a>',
                'cmd.asp?act=vrs">[\u67e5\u770b\u6743\u9650]</a>',
            )
            text = text.replace(
                'type="submit" value="" name="btnPost"',
                'type="submit" value="\u63d0\u4ea4" name="btnPost"',
            )
            text = text.replace(
                '<h4 id="BlogPowerBy">Powered By </h4>',
                '<h4 id="BlogPowerBy">Powered By '
                '<a href="http://www.zblogcn.com/" title="RainbowSoft Z-Blog">'
                "Z-Blog 2.3 Avengers Build 180518</a></h4>",
            )
            text = text.replace(
                '<h3 id="BlogCopyRight"></h3>',
                f'<h3 id="BlogCopyRight">{copyright_text}</h3>',
            )
        target.write_text(text, encoding="utf-8")


def patch_js(root: Path) -> None:
    """Coordinate asynchronous sidebar and autoinfo completion without races."""
    target = root / "zb_system/FUNCTION/c_html_js_add.asp"
    text = target.read_text(encoding="utf-8-sig")
    if "function TryAutoinfoComplete()" in text:
        return
    text = replace_once(
        text,
        """var strBatchCount="";

$(document).ready(function(){""",
        """var strBatchCount="";
var strAutoinfoScript=null;
var bolAutoinfoSidebarReady=false;
var bolAutoinfoCompleted=false;

function TryAutoinfoComplete(){
\tif(!bolAutoinfoCompleted&&bolAutoinfoSidebarReady&&strAutoinfoScript!==null){
\t\tbolAutoinfoCompleted=true;
\t\t$.globalEval(strAutoinfoScript);
\t\tAutoinfoComplete();
\t}
}

$(document).ready(function(){""",
        str(target),
    )
    text = replace_once(
        text,
        """\tsidebarloaded.add(function(){
\t\tif(GetCookie("username")!=""&&GetCookie("password")!=""){$.getScript("<%=BlogHost%>zb_system/function/c_html_js.asp?act=autoinfo",function(){AutoinfoComplete();})}else{AutoinfoComplete();}
\t});""",
        """\tvar bolRequestAutoinfo=!!GetCookie("username")&&!!GetCookie("password");
\tsidebarloaded.add(function(){
\t\tbolAutoinfoSidebarReady=true;
\t\tif(bolRequestAutoinfo){
\t\t\tTryAutoinfoComplete();
\t\t}else if(!bolAutoinfoCompleted){
\t\t\tbolAutoinfoCompleted=true;
\t\t\tAutoinfoComplete();
\t\t}
\t});
\tif(bolRequestAutoinfo){
\t\t$.ajax({url:"<%=BlogHost%>zb_system/function/c_html_js.asp?act=autoinfo",dataType:"text",cache:true,success:function(script){
\t\t\tstrAutoinfoScript=script;
\t\t\tTryAutoinfoComplete();
\t\t}});
\t}""",
        str(target),
    )
    target.write_text(text, encoding="utf-8")


def patch_template_cache_validation(root: Path) -> None:
    """Reject incomplete template globals before accepting a persisted cache."""
    target = root / "zb_system/FUNCTION/c_system_base.asp"
    text = target.read_text(encoding="utf-8-sig")
    if "Egret ASP: validate cached template globals" not in text:
        old = """\t\tIf IsEmpty(TemplateTagsValue)=False And IsEmpty(TemplateTagsValue)=False And IsEmpty(TemplatesName)=False And IsEmpty(TemplatesContent)=False Then
\t\t\tExit Function
\t\tEnd If"""
        new = """\t\t' Egret ASP: validate cached template globals before using them.
\t\tDim bolTemplateCacheValid, bolHasTheme, bolHasCss, bolHasTitle, bolHasLanguage, bolHasSubmit
\t\tbolTemplateCacheValid=False
\t\tbolHasTheme=False
\t\tbolHasCss=False
\t\tbolHasTitle=False
\t\tbolHasLanguage=False
\t\tbolHasSubmit=False
\t\tIf IsArray(TemplateTagsName) And IsArray(TemplateTagsValue) And IsArray(TemplatesName) And IsArray(TemplatesContent) Then
\t\t\tFor ii=0 To UBound(TemplateTagsName)
\t\t\t\tIf CStr(TemplateTagsName(ii))="ZC_BLOG_THEME" And Len(CStr(TemplateTagsValue(ii)))>0 Then bolHasTheme=True
\t\t\t\tIf CStr(TemplateTagsName(ii))="ZC_BLOG_CSS" And Len(CStr(TemplateTagsValue(ii)))>0 Then bolHasCss=True
\t\t\t\tIf CStr(TemplateTagsName(ii))="ZC_BLOG_TITLE" And Len(CStr(TemplateTagsValue(ii)))>0 Then bolHasTitle=True
\t\t\t\tIf CStr(TemplateTagsName(ii))="ZC_BLOG_LANGUAGE" And Len(CStr(TemplateTagsValue(ii)))>0 Then bolHasLanguage=True
\t\t\t\tIf CStr(TemplateTagsName(ii))="ZC_MSG087" And Len(CStr(TemplateTagsValue(ii)))>0 Then bolHasSubmit=True
\t\t\tNext
\t\t\tbolTemplateCacheValid=bolHasTheme And bolHasCss And bolHasTitle And bolHasLanguage And bolHasSubmit
\t\tEnd If
\t\tIf bolTemplateCacheValid Then Exit Function"""
        text = replace_once(text, old, new, str(target))
    text = text.replace(
        'Call Execute("aryTemplateTagsValue(a+a2+a3+e+j)=ZC_MSG" & i)',
        'aryTemplateTagsValue(a+a2+a3+e+j)=Eval("ZC_MSG" & i)',
    )
    text = text.replace(
        'Call Execute("aryTemplateTagsValue(a+a2+a3+e+d+j)="& t(j))',
        'aryTemplateTagsValue(a+a2+a3+e+d+j)=Eval(t(j))',
    )
    target.write_text(text, encoding="utf-8")


def patch_platform_detection(root: Path) -> None:
    """Classify mobile/tablet clients from capabilities and specific UA tokens."""
    target = root / "zb_users/PLUGIN/Wap/script/pad.js"
    text = target.read_text(encoding="utf-8-sig")
    old = """\tvar ua=navigator.userAgent.toLowerCase();
\tif(/android|adr|blink/.test(ua)){
\t\t$.android=true;


\t}
\telse if(/ipad|iphone|ipod|ios/.test(ua)){
\t\t$.ios=true;
\t}
\telse if(/windows phone|iemobile|wpdesktop/.test(ua)){
\t\t$.windowsphone=true;
\t}
\telse if(/blackberry|bb10|playbook/.test(ua)){
\t\t$.blackberry=true;
\t}

\tif(window.screen.width>=1024||window.screen.height>=1024){
\t\t$.pad=true;
\t\t$.mobile=false;
\t}"""
    new = """\t// Egret ASP: classify by platform capabilities, never by desktop screen size.
\tvar ua=navigator.userAgent.toLowerCase();
\tvar touchPoints=navigator.maxTouchPoints||0;
\tvar isiPadOS=/ipad/.test(ua)||(/macintosh/.test(ua)&&touchPoints>1);
\tvar isAndroid=/android|\\badr\\b/.test(ua);
\tvar isTablet=isiPadOS
\t\t||/tablet|kindle|silk|playbook|rim\\stablet|gt\\-p|gt\\-n|sm\\-t|nexus\\s(?:7|9|10)|xoom|meego/.test(ua)
\t\t||(isAndroid&&!/mobile/.test(ua));
\tvar isPhone=/iphone|ipod|windows phone|iemobile|blackberry|bb10|mobile/.test(ua)
\t\t||(isAndroid&&/mobile/.test(ua));

\t$.android=isAndroid;
\t$.ios=isiPadOS||/iphone|ipod|ios/.test(ua);
\t$.windowsphone=/windows phone|iemobile|wpdesktop/.test(ua);
\t$.blackberry=/blackberry|bb10|playbook/.test(ua);
\t$.pad=isTablet;
\t$.mobile=isTablet||isPhone;"""
    if "Egret ASP: classify by platform capabilities" not in text:
        text = replace_once(text, old, new, str(target))
        target.write_text(text, encoding="utf-8")

    include = root / "zb_users/PLUGIN/Wap/include.asp"
    include_text = include.read_text(encoding="utf-8-sig")
    old_pad_list = 'Pad_List="android|iphone|ipad|windows\\sphone|kindle|gt\\-p|gt\\-n|meego"'
    new_pad_list = (
        'Pad_List="ipad|kindle|silk|rim\\stablet|playbook|tablet|'
        'gt\\-p|gt\\-n|sm\\-t|nexus\\s(7|9|10)|xoom|meego"'
    )
    include_text = include_text.replace(old_pad_list, new_pad_list)
    include.write_text(include_text, encoding="utf-8")


def patch_upload_settings(root: Path) -> None:
    """Allow inert C headers and align the upload limit with the 32 MiB gateway."""
    target = root / "zb_users/c_option.asp"
    text = target.read_text(encoding="utf-8-sig")
    prefix = 'ZC_UPLOAD_FILETYPE="'
    start = text.find(prefix)
    if start < 0:
        raise RuntimeError(f"{target}: ZC_UPLOAD_FILETYPE setting was not found")
    value_start = start + len(prefix)
    value_end = text.find('"', value_start)
    if value_end < 0:
        raise RuntimeError(f"{target}: malformed ZC_UPLOAD_FILETYPE setting")
    file_types = text[value_start:value_end].split("|")
    if "h" not in file_types:
        file_types.append("h")
        text = text[:value_start] + "|".join(file_types) + text[value_end:]
    size_prefix = "ZC_UPLOAD_FILESIZE="
    size_start = text.find(size_prefix)
    if size_start < 0:
        raise RuntimeError(f"{target}: ZC_UPLOAD_FILESIZE setting was not found")
    size_value_start = size_start + len(size_prefix)
    size_value_end = text.find("\n", size_value_start)
    if size_value_end < 0:
        size_value_end = len(text)
    text = text[:size_value_start] + "33554432" + text[size_value_end:]
    target.write_text(text, encoding="utf-8")


def patch_upload_error_statuses(root: Path) -> None:
    """Return protocol-specific client errors for rejected uploads."""
    target = root / "zb_system/FUNCTION/c_function.asp"
    text = target.read_text(encoding="utf-8-sig")
    old = """\tIf id=2 Then
\t\tResponse.Status="404 Not Found"
\tElse
\t\tResponse.Status="500 Internal Server Error"
\tEnd If"""
    new = """\tIf id=2 Then
\t\tResponse.Status="404 Not Found"
\tElseIf id=26 Then
\t\tResponse.Status="415 Unsupported Media Type"
\tElseIf id=27 Then
\t\tResponse.Status="413 Payload Too Large"
\tElse
\t\tResponse.Status="500 Internal Server Error"
\tEnd If"""
    if "413 Payload Too Large" not in text:
        text = replace_once(text, old, new, str(target))
        target.write_text(text, encoding="utf-8")


def main() -> int:
    """Apply every idempotent patch to explicit roots or standard build copies."""
    roots = [Path(arg) for arg in sys.argv[1:]]
    if not roots:
        roots = [Path("examples/zblog-mysql-openasp"), Path("build/zblogasp-run")]
    for root in roots:
        patch_tags(root)
        patch_feed_cache(root)
        patch_include_path_case(root)
        patch_js(root)
        patch_template_cache_validation(root)
        patch_platform_detection(root)
        patch_upload_settings(root)
        patch_upload_error_statuses(root)
        repair_sidebar(root)
        restore_safe_theme_markup(root)
        patch_sidebar_style(root)
        patch_admin_sidebar_style(root)
        print(f"patched {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
