@echo off
rem Start WebLinux: serve the page and open it in the default browser.
rem Needs Node.js (https://nodejs.org). Close this window to stop.
cd /d "%~dp0"
where node >nul 2>nul || (echo Node.js is not installed: get it from https://nodejs.org & pause & exit /b 1)
start "" http://127.0.0.1:8080/
node server.js 8080
