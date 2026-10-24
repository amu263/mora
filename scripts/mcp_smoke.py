"""Offline stdio conformance smoke test. Never touches a real account."""
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
messages=[
    {'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'mora-test','version':'1'}}},
    {'jsonrpc':'2.0','method':'notifications/initialized'},
    {'jsonrpc':'2.0','id':2,'method':'tools/list'},
    {'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'music_get_status','arguments':{}}},
    {'jsonrpc':'2.0','id':4,'method':'tools/call','params':{'name':'music_not_a_tool','arguments':{}}},
    {'jsonrpc':'2.0','id':5,'method':'tools/call','params':{'name':'music_search','arguments':{'query':'x','limit':999}}},
    {'jsonrpc':123,'id':6,'method':'ping'},
]
live='--live' in sys.argv
if live:
    messages.append({'jsonrpc':'2.0','id':7,'method':'tools/call','params':{'name':'music_search','arguments':{'query':'夜的钢琴曲','kind':'album','limit':2}}})
payload='\n'.join(json.dumps(m,ensure_ascii=False) for m in messages)+'\n'
result=subprocess.run([str(ROOT/'build/mora.exe'),'--mcp','--data-dir',str(ROOT/'build/mcp-test-profile')],input=payload,encoding='utf-8',capture_output=True,timeout=20)
assert result.returncode==0,result.stderr
assert '\x1b' not in result.stdout
responses=[json.loads(line) for line in result.stdout.splitlines()]
assert len(responses)==(7 if live else 6),responses
assert responses[0]['result']['protocolVersion']=='2025-11-25'
assert {t['name'] for t in responses[1]['result']['tools']}=={'music_search','music_open_collection','music_play','music_control','music_now_playing','music_get_lyrics','music_recommend','music_play_random','music_list_playlists','music_set_like','music_add_to_playlist','music_get_status','music_queue_list','music_queue_edit'}
assert responses[2]['result']['structuredContent']['playerSessionAttached'] is False
assert responses[3]['error']['code']==-32602
assert responses[4]['result']['isError'] is True
assert responses[5]['error']['code']==-32600
if live:
    assert not responses[6]['result']['isError'],responses[6]
    assert len(responses[6]['result']['structuredContent']['items'])==2
report={'passed':True,'checks':10 if live else 8,'protocol':'stdio JSON-RPC','realNetworkUsed':live}
(ROOT/('build/mcp-live-smoke.json' if live else 'build/mcp-smoke.json')).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report))
