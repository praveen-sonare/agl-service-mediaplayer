--[[
 Copyright 2019 Konsulko Group

 author:Edi Feschiyan <edi.feschiyan@konsulko.com>

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
--]]



_AFT.testVerbStatusSuccess('testPlaylistSuccess','mediaplayer','playlist', {})

_AFT.testVerbStatusSuccess('testControlsPlaySuccess','mediaplayer','controls', {value="play"})
_AFT.testVerbStatusSuccess('testControlsPauseSuccess','mediaplayer','controls', {value="pause"})
_AFT.testVerbStatusSuccess('testControlsPreviousSuccess','mediaplayer','controls', {value="previous"})
_AFT.testVerbStatusSuccess('testControlsNextSuccess','mediaplayer','controls', {value="next"})
_AFT.testVerbStatusSuccess('testControlsSeekSuccess','mediaplayer','controls', {value="seek", position=10000})
_AFT.testVerbStatusSuccess('testControlsFastForwardSuccess','mediaplayer','controls', {value="fast-forward", position=10000})
_AFT.testVerbStatusSuccess('testControlsRewindSuccess','mediaplayer','controls', {value="rewind", position=10000})
_AFT.testVerbStatusSuccess('testControlsPickTrackSuccess','mediaplayer','controls', {value="pick-track", index=1})
_AFT.testVerbStatusSuccess('testControlsVolumeSuccess','mediaplayer','controls', {value="volume", volume=10})
_AFT.testVerbStatusSuccess('testControlsLoopEnableSuccess','mediaplayer','controls', {value="loop", state="on"})
_AFT.testVerbStatusSuccess('testControlsLoopDisableSuccess','mediaplayer','controls', {value="loop", state="off"})


_AFT.testVerbStatusSuccess('testSubscribePlaylistSuccess','mediaplayer','subscribe', {value="playlist"})
_AFT.testVerbStatusSuccess('testSubscribeMetadataSuccess','mediaplayer','subscribe', {value="metadata"})

_AFT.testVerbStatusSuccess('testUnsubscribePlaylistSuccess','mediaplayer','unsubscribe', {value="playlist"})
_AFT.testVerbStatusSuccess('testUnsubscribeMetadataSuccess','mediaplayer','unsubscribe', {value="metadata"})
