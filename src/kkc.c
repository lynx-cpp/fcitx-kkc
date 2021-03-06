/***************************************************************************
 *   Copyright (C) YEAR~YEAR by Your Name                                  *
 *   your-email@address.com                                                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcitx/ime.h>
#include <fcitx-config/fcitx-config.h>
#include <fcitx-config/xdg.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utils.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/instance.h>
#include <fcitx/context.h>
#include <fcitx/module.h>
#include <fcitx/hook.h>
#include <fcitx/candidate.h>
#include <libkkc/libkkc.h>
#include <libintl.h>

#include "config.h"
#include "kkc-internal.h"

static void *FcitxKkcCreate(FcitxInstance *instance);
static void FcitxKkcDestroy(void *arg);
static void FcitxKkcReloadConfig(void *arg);
CONFIG_DEFINE_LOAD_AND_SAVE(Kkc, FcitxKkcConfig, "fcitx-kkc")
DECLARE_ADDFUNCTIONS(Kkc)

#define _FcitxKeyState_Release (1 << 30)

FCITX_DEFINE_PLUGIN(fcitx_kkc, ime2, FcitxIMClass2) = {
    FcitxKkcCreate,
    FcitxKkcDestroy,
    FcitxKkcReloadConfig,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

boolean FcitxKkcInit(void *arg); /**< FcitxIMInit */
void FcitxKkcResetIM(void *arg); /**< FcitxIMResetIM */
INPUT_RETURN_VALUE FcitxKkcDoInput(void *arg, FcitxKeySym, unsigned int); /**< FcitxIMDoInput */
INPUT_RETURN_VALUE FcitxKkcDoReleaseInput(void *arg, FcitxKeySym, unsigned int); /**< FcitxIMDoInput */
INPUT_RETURN_VALUE FcitxKkcGetCandWords(void *arg); /**< FcitxIMGetCandWords */
void FcitxKkcSave(void *arg); /**< FcitxIMSave */
void FcitxKkcApplyConfig(FcitxKkc* kkc);
void FcitxKkcResetHook(void *arg);

typedef struct _KkcStatus {
    const char* icon;
    const char* label;
    const char* description;
} KkcStatus;

KkcStatus input_mode_status[] = {
    {"",  "\xe3\x81\x82", N_("Hiragana") },
    {"", "\xe3\x82\xa2", N_("Katakana") },
    {"", "\xef\xbd\xb1", N_("Half width Katakana") },
    {"", "A", N_("Direct input") },
    {"", "\xef\xbc\xa1", N_("Wide latin") },
};

const char* FcitxKkcGetInputModeIconName(void* arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    return input_mode_status[kkc_context_get_input_mode(kkc->context)].icon;
}

void FcitxKkcUpdateInputModeMenu(struct _FcitxUIMenu *menu)
{
    FcitxKkc *kkc = (FcitxKkc*) menu->priv;
    menu->mark = kkc_context_get_input_mode(kkc->context);
}

boolean FcitxKkcInputModeMenuAction(struct _FcitxUIMenu *menu, int index)
{
    FcitxKkc *kkc = (FcitxKkc*) menu->priv;
    kkc_context_set_input_mode(kkc->context, (KkcInputMode) index);
    return true;
}

void FcitxKkcUpdateInputMode(FcitxKkc* kkc)
{
    KkcInputMode mode = kkc_context_get_input_mode(kkc->context);
    FcitxUISetStatusString(kkc->owner,
                           "kkc-input-mode",
                           _(input_mode_status[mode].label),
                           _(input_mode_status[mode].description));
}

static void  _kkc_input_mode_changed_cb                (GObject    *gobject,
                                                        GParamSpec *pspec,
                                                        gpointer    user_data)
{
    FcitxKkc *kkc = (FcitxKkc*) user_data;
    FcitxKkcUpdateInputMode(kkc);
}

static void*
FcitxKkcCreate(FcitxInstance *instance)
{
    FcitxKkc *kkc = fcitx_utils_new(FcitxKkc);
    bindtextdomain("fcitx-kkc", LOCALEDIR);
    kkc->owner = instance;

    g_type_init();
    kkc_init();

    KkcLanguageModel* model = kkc_language_model_load("sorted3", NULL);
    if (!model) {
        free(kkc);
        return NULL;
    }

    FcitxXDGMakeDirUser("kkc/rules");
    FcitxXDGMakeDirUser("kkc/dictionary");

    kkc->model = model;
    kkc->context = kkc_context_new(model);
    KkcDictionaryList* dictionaries = kkc_context_get_dictionaries(kkc->context);
    KkcSystemSegmentDictionary* dict = kkc_system_segment_dictionary_new("/usr/share/skk/SKK-JISYO.L", "EUC-JP", NULL);
    char* path = NULL;
    FcitxXDGGetFileUserWithPrefix("kkc", "dictionary", NULL, &path);
    KkcUserDictionary* userdict = kkc_user_dictionary_new(path, NULL);
    kkc_dictionary_list_add(dictionaries, KKC_DICTIONARY(dict));
    kkc_dictionary_list_add(dictionaries, KKC_DICTIONARY(userdict));
    kkc_context_set_punctuation_style(kkc->context, KKC_PUNCTUATION_STYLE_JA_JA);
    kkc_context_set_input_mode(kkc->context, KKC_INPUT_MODE_HIRAGANA);

    KkcRule* rule = kkc_rule_new(kkc_rule_find_rule("default"), NULL);

    kkc_context_set_typing_rule(kkc->context, rule);

    if (!KkcLoadConfig(&kkc->config)) {
        free(kkc);
        return NULL;
    }

    FcitxKkcApplyConfig(kkc);

    FcitxIMIFace iface;
    memset(&iface, 0, sizeof(FcitxIMIFace));
    iface.Init = FcitxKkcInit;
    iface.DoInput = FcitxKkcDoInput;
    iface.DoReleaseInput = FcitxKkcDoReleaseInput;
    iface.GetCandWords = FcitxKkcGetCandWords;
    iface.Save = FcitxKkcSave;
    iface.ResetIM = FcitxKkcResetIM;

    FcitxInstanceRegisterIMv2(instance, kkc, "kkc", _("Kana Kanji"), "kkc", iface, 1, "ja");


#define INIT_MENU(VARNAME, NAME, I18NNAME, STATUS_NAME, STATUS_ARRAY, SIZE) \
    do { \
        FcitxUIRegisterComplexStatus(instance, kkc, \
            STATUS_NAME, \
            I18NNAME, \
            I18NNAME, \
            NULL, \
            FcitxKkcGet##NAME##IconName \
        ); \
        FcitxMenuInit(&VARNAME); \
        VARNAME.name = strdup(I18NNAME); \
        VARNAME.candStatusBind = strdup(STATUS_NAME); \
        VARNAME.UpdateMenu = FcitxKkcUpdate##NAME##Menu; \
        VARNAME.MenuAction = FcitxKkc##NAME##MenuAction; \
        VARNAME.priv = kkc; \
        VARNAME.isSubMenu = false; \
        int i; \
        for (i = 0; i < SIZE; i ++) \
            FcitxMenuAddMenuItem(&VARNAME, _(STATUS_ARRAY[i].label), MENUTYPE_SIMPLE, NULL); \
        FcitxUIRegisterMenu(instance, &VARNAME); \
        FcitxUISetStatusVisable(instance, STATUS_NAME, false); \
    } while(0)

    INIT_MENU(kkc->inputModeMenu, InputMode, _("Input Mode"), "kkc-input-mode", input_mode_status, KKC_INPUT_MODE_DIRECT + 1);

    kkc->handler = g_signal_connect(kkc->context, "notify::input-mode", G_CALLBACK(_kkc_input_mode_changed_cb), kkc);
    FcitxKkcUpdateInputMode(kkc);

    kkc_context_set_input_mode(kkc->context, kkc->config.initialInputMode);

    FcitxIMEventHook hk;
    hk.arg = kkc;
    hk.func = FcitxKkcResetHook;
    FcitxInstanceRegisterResetInputHook(instance, hk);

    FcitxKkcAddFunctions(instance);
    return kkc;
}

INPUT_RETURN_VALUE FcitxKkcDoInputReal(void* arg, FcitxKeySym sym, unsigned int state)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    state = state & (FcitxKeyState_SimpleMask | _FcitxKeyState_Release);
    KkcCandidateList* kkcCandidates = kkc_context_get_candidates(kkc->context);

    if (kkc_candidate_list_get_page_visible(kkcCandidates)) {
        if (FcitxHotkeyIsHotKeyDigit(sym, state)) {
            return IRV_TO_PROCESS;
        } else if (FcitxHotkeyIsHotKey(sym, state, kkc->config.prevPageKey)) {
            return IRV_TO_PROCESS;
        } else if (FcitxHotkeyIsHotKey(sym, state, kkc->config.nextPageKey)) {
            return IRV_TO_PROCESS;
        } else if (FcitxHotkeyIsHotKey(sym, state, kkc->config.cursorUpKey)) {
            if (!(state & _FcitxKeyState_Release)) {
                KkcCandidateList* kkcCandidates = kkc_context_get_candidates(kkc->context);
                kkc_candidate_list_cursor_up(kkcCandidates);
                return IRV_DISPLAY_CANDWORDS;
            } else {
                return IRV_TO_PROCESS;
            }
        } else if (FcitxHotkeyIsHotKey(sym, state, kkc->config.cursorDownKey)) {
            if (!(state & _FcitxKeyState_Release)) {
                KkcCandidateList* kkcCandidates = kkc_context_get_candidates(kkc->context);
                kkc_candidate_list_cursor_down(kkcCandidates);
                return IRV_DISPLAY_CANDWORDS;
            } else {
                return IRV_TO_PROCESS;
            }
        }
    }

    FcitxInputState* input = FcitxInstanceGetInputState(kkc->owner);
    uint32_t keycode = FcitxInputStateGetKeyCode(input);
    KkcKeyEvent* key = kkc_key_event_new_from_x_event(sym, keycode, state);
    if (!key) {
        return IRV_TO_PROCESS;
    }

    gboolean retval = kkc_context_process_key_event(kkc->context, key);

    g_object_unref(key);
    if (retval) {
        return IRV_DISPLAY_CANDWORDS;
    }
    return IRV_TO_PROCESS;
}

boolean FcitxKkcInit(void* arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    boolean flag = true;
    FcitxInstanceSetContext(kkc->owner, CONTEXT_IM_KEYBOARD_LAYOUT, "jp");
    FcitxInstanceSetContext(kkc->owner, CONTEXT_DISABLE_AUTOENG, &flag);
    FcitxInstanceSetContext(kkc->owner, CONTEXT_DISABLE_QUICKPHRASE, &flag);
    FcitxInstanceSetContext(kkc->owner, CONTEXT_DISABLE_FULLWIDTH, &flag);
    FcitxInstanceSetContext(kkc->owner, CONTEXT_DISABLE_AUTO_FIRST_CANDIDATE_HIGHTLIGHT, &flag);

    FcitxInstanceSetContext(kkc->owner, CONTEXT_ALTERNATIVE_PREVPAGE_KEY, kkc->config.prevPageKey);
    FcitxInstanceSetContext(kkc->owner, CONTEXT_ALTERNATIVE_NEXTPAGE_KEY, kkc->config.nextPageKey);
    return true;
}

INPUT_RETURN_VALUE FcitxKkcDoInput(void* arg, FcitxKeySym _sym, unsigned int _state)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    FcitxInputState* input = FcitxInstanceGetInputState(kkc->owner);
    FcitxKeySym sym = (FcitxKeySym) FcitxInputStateGetKeySym(input);
    uint32_t state = FcitxInputStateGetKeyState(input);
    return FcitxKkcDoInputReal(arg, sym, state);
}

INPUT_RETURN_VALUE FcitxKkcDoReleaseInput(void* arg, FcitxKeySym _sym, unsigned int _state)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    FcitxInputState* input = FcitxInstanceGetInputState(kkc->owner);
    FcitxKeySym sym = (FcitxKeySym) FcitxInputStateGetKeySym(input);
    uint32_t state = FcitxInputStateGetKeyState(input);
    state |= _FcitxKeyState_Release;
    return FcitxKkcDoInputReal(arg, sym, state);
}

INPUT_RETURN_VALUE FcitxKkcGetCandWord(void* arg, FcitxCandidateWord* word)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    KkcCandidateList* kkcCandidates = kkc_context_get_candidates(kkc->context);
    int idx = *((int*)word->priv);
    gboolean retval = kkc_candidate_list_select_at(kkcCandidates, idx % kkc->config.pageSize);
    if (retval) {
        return IRV_DISPLAY_CANDWORDS;
    }

    return IRV_TO_PROCESS;
}

boolean FcitxKkcPaging(void* arg, boolean prev) {
    FcitxKkc *kkc = (FcitxKkc*)arg;
    KkcCandidateList* skkCandList = kkc_context_get_candidates(kkc->context);
    if (kkc_candidate_list_get_page_visible(skkCandList)) {
        if (prev)
            kkc_candidate_list_page_up(skkCandList);
        else
            kkc_candidate_list_page_down(skkCandList);
        FcitxKkcGetCandWords(kkc);
        return true;
    }
    return false;
}

INPUT_RETURN_VALUE FcitxKkcGetCandWords(void* arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    FcitxInputState* input = FcitxInstanceGetInputState(kkc->owner);
    FcitxCandidateWordList* candList = FcitxInputStateGetCandidateList(input);
    FcitxMessages* clientPreedit = FcitxInputStateGetClientPreedit(input);
    FcitxMessages* preedit = FcitxInputStateGetPreedit(input);
    FcitxInstanceCleanInputWindow(kkc->owner);

    FcitxMessages* message = FcitxInstanceICSupportPreedit(kkc->owner, FcitxInstanceGetCurrentIC(kkc->owner)) ? clientPreedit : preedit;

    FcitxCandidateWordSetChoose(candList, DIGIT_STR_CHOOSE);
    FcitxCandidateWordSetPageSize(candList, kkc->config.pageSize);
    FcitxCandidateWordSetLayoutHint(candList, kkc->config.candidateLayout);
    FcitxInputStateSetShowCursor(input, true);

    KkcSegmentList* segments = kkc_context_get_segments(kkc->context);
    if (kkc_segment_list_get_cursor_pos(segments) >= 0) {
        int i = 0;
        int offset = 0;
        for (i = 0; i < kkc_segment_list_get_size(segments); i ++) {
            KkcSegment* segment = kkc_segment_list_get(segments, i);
            const gchar* str = kkc_segment_get_output(segment);
            FcitxMessageType messageType = MSG_INPUT;
            if (i < kkc_segment_list_get_cursor_pos(segments)) {
                offset += strlen(str);
            }
            if (i == kkc_segment_list_get_cursor_pos(segments)) {
                messageType = (FcitxMessageType) (MSG_HIGHLIGHT | MSG_OTHER);
            }
            FcitxMessagesAddMessageAtLast(message, messageType, "%s", str);
        }

        if (message == clientPreedit) {
            FcitxInputStateSetClientCursorPos(input, offset);
        } else {
            FcitxInputStateSetCursorPos(input, offset);
        }
    } else {
        gchar* str = kkc_context_get_input(kkc->context);
        FcitxMessagesAddMessageAtLast(message, MSG_INPUT, "%s", str);

        if (message == clientPreedit) {
            FcitxInputStateSetClientCursorPos(input, strlen(str));
        } else {
            FcitxInputStateSetCursorPos(input, strlen(str));
        }
        g_free(str);
    }

    KkcCandidateList* kkcCandidates = kkc_context_get_candidates(kkc->context);
    if (kkc_candidate_list_get_page_visible(kkcCandidates)) {
        int i, j;
        guint size = kkc_candidate_list_get_size(kkcCandidates);
        gint cursor_pos = kkc_candidate_list_get_cursor_pos(kkcCandidates);
        guint page_start = kkc_candidate_list_get_page_start(kkcCandidates);
        guint page_size = kkc_candidate_list_get_page_size(kkcCandidates);
        for (i = kkc_candidate_list_get_page_start(kkcCandidates), j = 0; i < size; i ++, j++) {
            FcitxCandidateWord word;
            word.callback = FcitxKkcGetCandWord;
            word.extraType = MSG_OTHER;
            word.owner = kkc;
            int* id = fcitx_utils_new(int);
            *id = j;
            word.priv = id;
            word.strExtra = NULL;
            word.strExtra = MSG_TIPS;
            KkcCandidate* kkcCandidate = kkc_candidate_list_get(kkcCandidates, i);
            if (kkc->config.showAnnotation && kkc_candidate_get_annotation(kkcCandidate)) {
                fcitx_utils_alloc_cat_str(word.strExtra, " [", kkc_candidate_get_annotation(kkcCandidate), "]");
            }
            word.strWord = strdup(kkc_candidate_get_text(kkc_candidate_list_get(kkcCandidates, i)));
            if (i == cursor_pos) {
                word.wordType = MSG_CANDIATE_CURSOR;
            } else {
                word.wordType = MSG_OTHER;
            }

            FcitxCandidateWordAppend(candList, &word);
        }
        FcitxCandidateWordSetFocus(candList, cursor_pos - page_start);

        FcitxCandidateWordSetOverridePaging(candList,
                                            (cursor_pos - page_start) >= page_size,
                                            (size - page_start) / page_size != (cursor_pos - page_start) / page_size,
                                            FcitxKkcPaging,
                                            kkc,
                                            NULL);
    }

    if (kkc_context_has_output(kkc->context)) {
        gchar* str = kkc_context_poll_output(kkc->context);
        FcitxInstanceCommitString(kkc->owner, FcitxInstanceGetCurrentIC(kkc->owner), str);
        g_free(str);
    }

    return IRV_DISPLAY_CANDWORDS;
}

void FcitxKkcResetIM(void* arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    kkc_context_reset(kkc->context);
}

void FcitxKkcSave(void* arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    kkc_dictionary_list_save(kkc_context_get_dictionaries(kkc->context));
}



static void
FcitxKkcDestroy(void *arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;

    g_signal_handler_disconnect(kkc->context, kkc->handler);
    g_object_unref(kkc->context);

    free(kkc);
}

static void
FcitxKkcReloadConfig(void *arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    KkcLoadConfig(&kkc->config);
    FcitxKkcApplyConfig(kkc);
}

void FcitxKkcApplyConfig(FcitxKkc* kkc)
{
    KkcCandidateList* kkcCandidates = kkc_context_get_candidates(kkc->context);
    kkc_candidate_list_set_page_start(kkcCandidates, kkc->config.nTriggersToShowCandWin);
    kkc_candidate_list_set_page_size(kkcCandidates, kkc->config.pageSize);
    kkc_context_set_punctuation_style(kkc->context, kkc->config.punctuationStyle);
}

void FcitxKkcResetHook(void *arg)
{
    FcitxKkc *kkc = (FcitxKkc*)arg;
    FcitxIM* im = FcitxInstanceGetCurrentIM(kkc->owner);
#define RESET_STATUS(STATUS_NAME) \
    if (im && strcmp(im->uniqueName, "kkc") == 0) \
        FcitxUISetStatusVisable(kkc->owner, STATUS_NAME, true); \
    else \
        FcitxUISetStatusVisable(kkc->owner, STATUS_NAME, false);

    RESET_STATUS("kkc-input-mode")
}

#include "fcitx-kkc-addfunctions.h"
