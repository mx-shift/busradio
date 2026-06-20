@echo off
REM Copyright 2026 Rick Altherr
REM
REM Licensed under the Apache License, Version 2.0 (the "License");
REM you may not use this file except in compliance with the License.
REM You may obtain a copy of the License at
REM
REM     http://www.apache.org/licenses/LICENSE-2.0
REM
REM Unless required by applicable law or agreed to in writing, software
REM distributed under the License is distributed on an "AS IS" BASIS,
REM WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
REM See the License for the specific language governing permissions and
REM limitations under the License.

REM Build BUSRADIO and BDTUNE
REM Assumes TCC, TLINK, ML on PATH and INCLUDE, LIB env set; cwd = this folder
REM Outputs land in .\BUILD
if not exist BUILD\NUL md BUILD
echo --BUSRADIO-- > BUILD\BUILD.LOG
echo --TCC-- >> BUILD\BUILD.LOG
tcc -c -ms -I%INCLUDE% -nBUILD\ BUSRADIO.C >> BUILD\BUILD.LOG
echo --ML-- >> BUILD\BUILD.LOG
ml /c /nologo /FoBUILD\BRLOOPS.OBJ BRLOOPS.ASM >> BUILD\BUILD.LOG
echo --TLINK-- >> BUILD\BUILD.LOG
tlink /c %LIB%\C0S.OBJ BUILD\BUSRADIO.OBJ BUILD\BRLOOPS.OBJ, BUILD\BUSRADIO.EXE, BUILD\BUSRADIO.MAP, %LIB%\CS.LIB >> BUILD\BUILD.LOG
echo --BDTUNE-- >> BUILD\BUILD.LOG
echo --TCC-- >> BUILD\BUILD.LOG
tcc -c -ms -w-eff -I%INCLUDE% -nBUILD\ BDTUNE.C >> BUILD\BUILD.LOG
echo --TLINK-- >> BUILD\BUILD.LOG
tlink /c %LIB%\C0S.OBJ BUILD\BDTUNE.OBJ BUILD\BRLOOPS.OBJ, BUILD\BDTUNE.EXE, BUILD\BDTUNE.MAP, %LIB%\CS.LIB >> BUILD\BUILD.LOG
echo --DONE-- >> BUILD\BUILD.LOG
