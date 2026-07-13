@echo off
call "C:\Espressif\frameworks\esp-idf-v5.4.3\export.bat"
"C:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" "C:\Espressif\frameworks\esp-idf-v5.4.3\tools\idf.py" %*
