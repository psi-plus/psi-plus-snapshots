/*
 * adapter.js - Psi chatview themes adapter
 * Copyright (C) 2010-2017  Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

function psiThemeAdapter(chat) {
    chat.console("Psi adapter is ready");

    return {
    loadTheme : function(style) {
        const finishSetup = (html, js, scripts) => {
            var hasStyles = html.indexOf("%styles%") !== -1;
            var hasStylesVariant = html.indexOf("%stylesVariant%") !==  -1;
            var styles = '<link rel="stylesheet" href="/psi/themes/chatview/psi/psi.css" type="text/css">';
            var stylesVariant = `<link rel="stylesheet" href="${style}.css" type="text/css">`;
            if (style && !hasStylesVariant) {
                styles +=  stylesVariant;
            }
            html = html.replace("%scripts%", scripts + (hasStyles?"":"%styles%"));
            html = html.replace("%styles%", styles);
            if (hasStylesVariant) {
                html = html.replace("%stylesVariant%", style? stylesVariant : "");
            }
            srvLoader.setHtml(html);
            eval(js);
            srvLoader.finishThemeLoading();
        };

        if (chat.async) {
            srvLoader.getFileContents("index.html", function(html){
                // FIXME we have a lot of copies of this html everywhere. should be rewritten somehow
                // probably it's a good idea if adapter will send to Psi a list of required scripts
                var scripts = "<script src=\"/psi/themes/chatview/moment-with-locales.js\"></script>\n \
<script src=\"/psi/themes/chatview/util.js\"></script>\n \
<script src=\"/psi/themes/chatview/psi/adapter.js\"></script>\n \
<script src=\"/psi/static/qwebchannel.js\"></script>\n \
<script type=\"text/javascript\">\n \
    new QWebChannel(qt.webChannelTransport, function (channel) {\n \
        window.srvSession = channel.objects.srvSession;\n \
        window.srvUtil = channel.objects.srvUtil;\n \
        var shared = initPsiTheme().adapter.initSession();\n \
    });\n \
</script>";
                srvLoader.getFileContents("load.js", function(js){
                    finishSetup(html, js, scripts)
                })
            });
        } else {
            var html = srvLoader.getFileContents("index.html");
            var js = srvLoader.getFileContents("load.js");
            var scripts = `<script type="text/javascript">var shared = initPsiTheme().adapter.initSession();</script>`;
            finishSetup(html, js, scripts);
        }
    },
    initSession : function() {

        var trackbar = null;
        var inited = false;
        var proxy = null;
        var session = window.srvSession;

        var shared = {
            templates : {},
            server : window.srvUtil,
            session : session,
            isMuc : session.isMuc,
            accountId : window.srvSession.account,
            timeFormat : "HH:mm:ss",
            dateFormat : "LL",
            dateTimeFormat: "LL HH:mm:ss",
            scroller : null,
            varHandlers : {},
            prevGrouppingData : null,
            groupping : false,
            chatElement : null,
            chat : chat,

            TemplateVar : function(name, param) {
                this.name = name;
                this.param = param;
            },

            TemplateTimeVar : function(name, format) {
                this.name = name;
                this.formatter = new chat.DateTimeFormatter(format || (name == "date"? shared.dateFormat : shared.timeFormat));
            },

            TemplateTemplateVar : function(template_name) {
                this.template_name = template_name;
            },

            TemplateEscapeUriVar : function(name) {
                this.name = name; // cdata member name
            },

            Template : function(raw) {
                var splitted = raw.split(/(%[\w]+(?:\{[^\{]+\})?%)/), i;
                this.parts = [];

                for (i = 0; i < splitted.length; i++) {
                    var m = splitted[i].match(/%([\w]+)(?:\{([^\{]+)\})?%/);
                    if (m) {
                        switch (m[1]) {
                        case "date":
                        case "time": this.parts.push(new shared.TemplateTimeVar(m[1], m[2])); break;
                        case "template": this.parts.push(new shared.TemplateTemplateVar(m[2])); break;
                        case "escapeURI": this.parts.push(new shared.TemplateEscapeUriVar(m[2])); break;
                        default: this.parts.push(new shared.TemplateVar(m[1], m[2])); break;
                        }
                    } else {
                        this.parts.push(splitted[i]);
                    }
                }
            },

            appendHtml : function(html, scroll, nextEl) { //scroll[true|false|auto/other]
                if (typeof(scroll) == 'boolean') {
                    shared.scroller.atBottom = scroll;
                }
                var el;
                if (nextEl) {
                    el = chat.util.siblingHtml(nextEl, html);
                } else {
                    el = chat.util.appendHtml(shared.chatElement, html, shared.isMuc? shared.cdata.sender : "");
                }
                shared.scroller.invalidate();
                return el;
            },

            stopGroupping : function() {
                if (shared.prevGrouppingData) {
                    if (shared.prevGrouppingData.nextEl) {
                        shared.prevGrouppingData.nextEl.parentNode.removeChild(shared.prevGrouppingData.nextEl);
                    }
                    shared.prevGrouppingData = null;
                }
            },

            initTheme : function(config) {
                if (inited) {
                    chat.util.showCriticalError("Theme should not be inited twice. Something wrong with theme.")
                    return false;
                }
                var t = shared.templates;
                shared.chatElement = config.chatElement;
                shared.timeFormat = config.timeFormat || shared.timeFormat;
                shared.dateFormat = config.dateFormat || shared.dateFormat;
                shared.dateTimeFormat = config.dateTimeFormat || shared.dateTimeFormat;
                shared.scroller = config.scroller || new chat.WindowScroller(false);
                shared.groupping = config.groupping || shared.groupping;
                proxy = config.proxy;
                shared.varHandlers = config.varHandlers || {};
                shared.msgElPostProcess = config.postProcess;
                for (var tname in config.templates) {
                    if (config.templates[tname]) {
                        t[tname] = new shared.Template(config.templates[tname].trim());
                    }
                }
                t.message = t.message || "%message%";
                t.sys = t.sys || "%message%";
                t.sysMessage = t.sysMessage || t.sys;
                t.sysMessageUT = t.sysMessageUT || t.sysMessage;
                t.statusMessage = t.statusMessage || t.sysMessage;
                t.statusMessageUT = t.statusMessageUT || (t.statusMessage || t.sysMessageUT);
                t.sentMessage = t.sentMessage || t.message;
                t.receivedMessage = t.receivedMessage || t.message;
                t.receivedMessageGroupping = t.receivedMessageGroupping || t.messageGroupping;
                t.sentMessageGroupping = t.sentMessageGroupping || t.messageGroupping;
                t.lastMsgDate = t.lastMsgDate || t.sys;
                t.subject = t.subject || t.sys;
                t.urls = t.urls || t.sys;
                t.trackbar = t.trackbar || "<hr/>";
                //config.defaultAvatar && shared.server.setDefaultAvatar(config.defaultAvatar)
                //config.avatarSize && shared.server.setAvatarSize(config.avatarSize.width, config.avatarSize.height);
                inited = true;
            },
            checkNextOfGroup : function() {
                shared.cdata.nextOfGroup = !!(shared.prevGrouppingData &&
                                    (shared.prevGrouppingData.type == shared.cdata.type) &&
                                    (shared.prevGrouppingData.mtype == shared.cdata.mtype) &&
                                    (shared.prevGrouppingData.userid == shared.cdata.userid) &&
                                    (shared.prevGrouppingData.emote == shared.cdata.emote) &&
                                    (shared.prevGrouppingData.local == shared.cdata.local) &&
                                    (shared.cdata.time - shared.prevGrouppingData.time) < 30000);
                return shared.cdata.nextOfGroup;
            },

            addNick : function(uriEncodedNick) {
                //chat.console("Add nick:" + uriEncodedNick);
                var nick = decodeURIComponent(uriEncodedNick);
                shared.session.nickInsertClick(nick);
                return false;
            }
        };

        // internationalization
        function tr(text)
        {
            // TODO translate
            return text;
        }

        // accepts some templater object and object with 2 optional methods: "pre" and "post(text)"
        function proxyTemplate(template, handlers)
        {
            return {"toString": function(){
                if(handlers.pre) handlers.pre();
                var result = template.toString();
                if(handlers.post) result = handlers.post(result);
                return result;
            }}
        }

        shared.TemplateVar.prototype = {
            toString : function() {
                const varHandler = shared.varHandlers[this.name];
                if (varHandler) {
                    return varHandler();
                }
                var d = shared.cdata[this.name];
                if (this.name == "sender") { //may not be html
                    d = chat.util.escapeHtml(d);
                } else if (d instanceof Date) {
                    chat.console("WARNING: " + this.name + " isn't handled by TemplateTimeVar");
                    d = chat.util.dateFormat(d, shared.dateTimeFormat);
                } else if (this.name == "avatarurl") {
                    var url;
                    if (shared.cdata.local) {
                        url = session.localUserAvatar? session.localUserAvatar : chat.server.psiDefaultAvatarUrl;
                    } else {
                        if (shared.isMuc) {
                            url = shared.cdata.sender && (chat.util.avatarForNick(shared.cdata.sender) || chat.server.psiDefaultAvatarUrl);
                        }
                        if (!url) {
                            url = session.remoteUserAvatar? session.remoteUserAvatar : chat.server.psiDefaultAvatarUrl;
                        }
                    }
                    return url;
                } else if (this.name == "next") {
                    shared.cdata.nextEl = "nextMessagePH"+(1000+Math.floor(Math.random()*1000));
                    return '<div id="'+shared.cdata.nextEl +'"></div>';
                } else if (this.name == "message" && shared.cdata.id) {
                    return chat.util.replaceableMessage(session.isMuc, shared.cdata.local, shared.cdata.sender, shared.cdata.id, d);
                }
                return d || "";
            }
        }

        shared.TemplateTimeVar.prototype.toString = function() {
            return shared.cdata[this.name]?
                        this.formatter.format(shared.cdata[this.name]):
                        this.formatter.format(new Date());
        }

        shared.TemplateTemplateVar.prototype.toString = function() {
            var tt = shared.templates[this.template_name];
            return "" + tt;
        }

        shared.TemplateEscapeUriVar.prototype.toString = function() {
            return encodeURIComponent(shared.cdata[this.name]);
        }

        shared.Template.prototype.toString = function() {
            return this.parts.join("");
        }

        chat.adapter.receiveObject = function(data) {
            shared.cdata = data;
            if (!inited) {
                chat.util.showCriticalError("A try to output data while theme is not inited. output is impossible.\nCheck if your theme does not have errors.");
                return;
            }
            try {
                //shared.server.console(chat.util.props(data, true))
                var template;
                if (proxy && (template = proxy()) === false) { // proxy stopped processing
                    return; //we don't store shared.prevGrouppingData here, let's proxy do it if needed
                }
                if (data.type == "replace") {
                    if (chat.util.replaceMessage(shared.chatElement, session.isMuc, data.local, data.sender, data.replaceId, data.id, data.message)) {
                        shared.scroller.invalidate();
                        return;
                    }
                    data.type = "message";
                    data.mtype = "message";
                }
                if (data.type == "message") {
                    if (data.mtype != "message") {
                        shared.stopGroupping();
                    }
                    if (!template) switch (data.mtype) {
                        case "message":
                            if (shared.checkNextOfGroup()) {
                                template = data.local?shared.templates.sentMessageGroupping:shared.templates.receivedMessageGroupping;
                            }
                            if (!template) {
                                data.nextOfGroup = false; //can't group w/o template
                                template = data.local?shared.templates.sentMessage:shared.templates.receivedMessage;
                            }
                            break;
                        case "join":
                        case "part":
                        case "newnick":
                        case "status":
                            template = data.usertext?shared.templates.statusMessageUT:shared.templates.statusMessage;
                            break;
                        case "system":
                            template = data.usertext?shared.templates.sysMessageUT:shared.templates.sysMessage;
                            break;
                        case "lastDate":
                            template = shared.templates.lastMsgDate;
                            break;
                        case "subject": //its better to init with proper templates on start than do comparision like below
                            template = shared.templates.subject;
                            break;
                        case "urls":
                            var i, urls=[];
                            for (url in data.urls) {
                                urls.push('<a href="'+url+'">'+(data.urls[url]?chat.util.escapeHtml(data.urls[url]):url)+"</a>");
                            }
                            data["message"] = urls.join("<br/>");
                            template = shared.templates.urls;
                            break;
                    }
                    if (template) {
                        var el = shared.appendHtml(template.toString(), data.local?true:null, data.nextOfGroup?
                            shared.prevGrouppingData.nextEl:null); //force scroll on local messages
                        if (shared.msgElPostProcess) {
                            shared.msgElPostProcess(el);
                        }
                        shared.stopGroupping();// safe clean up previous data
                        if (shared.cdata.nextEl) { //convert to DOM
                            shared.cdata.nextEl = document.getElementById(shared.cdata.nextEl);
                            shared.prevGrouppingData = shared.cdata;
                        }
                    } else {
                        throw "Template not found";
                    }
                } else if (data.type == "trackbar") {
                    if (!trackbar) {
                        trackbar = document.createElement("div");
                        trackbar.innerHTML = shared.templates.trackbar.toString();
                    } else {
                        shared.chatElement.removeChild(trackbar);
                    }
                    shared.chatElement.appendChild(trackbar);
                    shared.scroller.invalidate();
                    shared.stopGroupping(); //groupping impossible
                } else if (data.type == "clear") {
                    shared.stopGroupping(); //groupping impossible
                    shared.chatElement.innerHTML = "";
                    trackbar = null;
                }
            } catch(e) {
                chat.util.showCriticalError("APPEND ERROR: " + e + "\n" + (e.stack?e.stack:"<no stack>"))
            }
        };

        shared.session.newMessage.connect(chat.receiveObject);
        shared.session.scrollRequested.connect((value) => {
                                                   if (shared.scroller && shared.scroller.cancel)
                                                       shared.scroller.cancel();
                                                   window.scrollBy(0, value);
                                               });

        chat.adapter.initSession = null;
        chat.adapter.loadTheme = null;
        //window.srvLoader = null;
        //window.chatServer = null;
        window.chatSession = null;


        function start() {
            try {
                window.psiimtheme = startPsiTheme(shared);
                chat.util.rereadOptions();
            } catch(e) {
                chat.util.showCriticalError("Failed to start: "+e+" "+e.stack);
            }
        }

        if (typeof(startPsiTheme) != "undefined") {
            chat.console("Starting theme");
            start();
        } else {
            chat.console("startPsiTheme is not defined. wait for \"load\" event to try again");
            window.addEventListener("load", start);
        }

        return shared;
    }
};

}
