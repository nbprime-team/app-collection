# editor.py
from hpprime import *
from state import EditorState
from render import render_editor
from input import poll_mouse, poll_key
from myprint import getPrograms, loadProgram
from selector import ProgramSelectorState, render_selector, poll_selector_events

def new_editor(src_code, lang='python', screen_w=320, screen_h=240):
    return EditorState(src_code, lang, screen_w, screen_h)

def poll_events(state):
    poll_mouse(state)
    poll_key(state)

dimgrob(2, 320, 240, 16777215)
dimgrob(3, 320, 240, 16777215)
eval('G3:=AFiles("bg.png")')
eval('RECT_P(G3,0,0,320,240,#60000000h);')

mode = 'selector'
selector_state = ProgramSelectorState()
editor_state = EditorState([""], lang = 'python')

while True:
    if mode == 'selector':
        poll_events(editor_state)
        render_editor(2, editor_state) 
        #result = poll_selector_events(selector_state)
        #render_selector(2, selector_state)
        blit(0, 0, 0, 2)