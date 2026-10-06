#!/usr/bin/env python3
"""Visor de espectro: version grafica de app_cli.
Corre en la Raspberry. La ventana se ve en la PC conectandose con ssh -X.
"""

import math
import os
import select
import sys
import threading

from PySide6.QtCore import QObject, Signal
from PySide6.QtWidgets import (QApplication, QFormLayout, QHBoxLayout, QLabel,
                               QMainWindow, QPushButton, QSpinBox, QVBoxLayout,
                               QWidget)
import pyqtgraph as pg

ESP32_DEV_CMD = "/dev/esp32link_cmd"
ESP32_DEV_DATA = "/dev/esp32link_data"

MAX_LINE = 128  # mismo valor que en 3_gpos/esp32_link.h
MAX_PUNTOS = 512
TIMEOUT_RESP_MS = 1000
TIMEOUT_LIMPIEZA_MS = 200
DB_MIN_INICIAL = -9  # valor con el que el eje de dB inicia, luego se agranda si no entra en ese rango

fd_cmd = -1
fd_data = -1

# Texto de cada ERR n
errores = ["",
           "Comando desconocido",
           "Error de sintaxis",
           "Valor fuera de rango",
           "Configuracion invalida para iniciar",
           "No aplica al estado actual"]

# Parametros de set/get: rango y unidad
parametros = {"frec_inicio": (10, 99999, "Hz"),
              "frec_final": (11, 100000, "Hz"),
              "puntos": (2, 512, ""),
              "tiempo": (10, 10000, "ms")}


class Barrido:

    def __init__(self):
        self.frec_inicio = self.frec_final = self.puntos = self.tiempo = 0
        self.pts = []  # lista de (freq, db)


barrido = Barrido()

# ===================== barrido =====================


def campos(linea):
    """'POINT i=3 freq=18 db=-0.29' -> {'i': '3', 'freq': '18', 'db': '-0.29'}"""
    return dict(c.split("=", 1) for c in linea.split() if "=" in c)


def procesar_datos(linea):
    """SWEEP empieza un barrido nuevo y POINT agrega un punto.
    Devuelve True cuando llega el ultimo punto del barrido."""
    try:
        c = campos(linea)
        if linea.startswith("SWEEP") and 1 <= int(c["puntos"]) <= MAX_PUNTOS:
            barrido.frec_inicio = int(c["frec_inicio"])
            barrido.frec_final = int(c["frec_final"])
            barrido.puntos = int(c["puntos"])
            barrido.tiempo = int(c["tiempo"])
            barrido.pts = []
        elif linea.startswith("POINT") and len(barrido.pts) < barrido.puntos:
            barrido.pts.append((int(c["freq"]), float(c["db"])))
            return int(c["i"]) == barrido.puntos - 1
    except (KeyError, ValueError):
        pass
    return False


class Monitor(QObject):
    """Pasa cada linea de data del hilo monitor al hilo de la ventana"""
    nueva_linea = Signal(str)


monitor = Monitor()


def hilo_monitor():
    """Hilo monitor: loop bloqueante de read() sobre data, arma las lineas
    y las manda a la ventana."""
    resto = b""
    while buf := os.read(fd_data, MAX_LINE):
        resto += buf
        *lineas, resto = resto.split(b"\n")
        for linea in lineas:
            monitor.nueva_linea.emit(linea.decode(errors="ignore"))

# ===================== comandos al ESP32 =====================


def esperar_respuesta():
    """Espera con poll() hasta TIMEOUT_RESP_MS a que haya respuesta en
    /dev/esp32link_cmd. Devuelve la linea o None si vencio el timeout."""
    p = select.poll()
    p.register(fd_cmd, select.POLLIN)
    # poll() duerme hasta que llegue la respuesta o por timeout.
    if not p.poll(TIMEOUT_RESP_MS):
        return None
    # Volvio por POLLIN: la linea ya esta en el fifo y el read no se bloquea
    return os.read(fd_cmd, MAX_LINE).decode(errors="ignore").strip()


def limpiar_cmd(timeout_ms):
    """Lee y descarta todo lo que haya en /dev/esp32link_cmd hasta que pasen
    timeout_ms sin que llegue nada."""
    p = select.poll()
    p.register(fd_cmd, select.POLLIN)
    while p.poll(timeout_ms):
        os.read(fd_cmd, MAX_LINE)


def enviar_comando(linea):
    """Manda un comando al ESP32 y devuelve el texto de la respuesta."""
    # Descarta respuestas viejas: la primera linea que llegue despues
    # del write tiene que ser la de este comando
    limpiar_cmd(0)

    try:
        os.write(fd_cmd, linea.encode())  # el driver agrega el '\n'
    except OSError as e:
        return f"write: {e}"

    resp = esperar_respuesta()
    if resp is None:
        return "sin respuesta del ESP32"
    if resp.startswith("ERR ") and resp[4:].isdigit():
        codigo = int(resp[4:])
        if 1 <= codigo <= 5:
            return f"ERR {codigo}: {errores[codigo]}"
    return resp

# ===================== ventana =====================


class Ventana(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Espectro - ESP32 link")
        self.resize(1050, 560)

        # Grafico: dB en funcion de la frecuencia, eje X logaritmico
        pg.setConfigOptions(antialias=True)
        self.grafico = pg.PlotWidget()
        self.grafico.setBackground("w")
        self.grafico.showGrid(x=True, y=True, alpha=0.3)
        self.grafico.setLabel("bottom", "Frecuencia (Hz)")
        self.grafico.setLabel("left", "Transferencia (dB)")
        self.grafico.setLogMode(x=True, y=False)
        # Eje X: numeros solo en las decadas y lineas de grilla sin numero en
        # 2..9 de cada decada. Las posiciones van en log10 por el eje logaritmico
        decadas = [(1, "10"), (2, "100"), (3, "1k"), (4, "10k"), (5, "100k")]
        intermedias = [(math.log10(m * 10 ** d), "") for d in range(1, 5) for m in range(2, 10)]
        self.grafico.getAxis("bottom").setTicks([decadas, intermedias])
        self.grafico.setYRange(DB_MIN_INICIAL, 0)
        self.curva = self.grafico.plot(pen=pg.mkPen(30, 100, 220, width=2))

        # Panel lateral: parametros (set/get), control del barrido y estado
        panel = QVBoxLayout()

        # Cada spinbox manda su set al terminar de editarlo (Enter o cambio de foco)
        form = QFormLayout()
        self.spinboxes = {}
        self.invalidos = set()  # parametros con un valor que no se pudo aplicar
        for nombre, (minimo, maximo, unidad) in parametros.items():
            spinbox = QSpinBox()
            # El spinbox acepta cualquier numero; el rango se valida en
            # aplicar_parametro para poder avisar cual es
            spinbox.setRange(0, 999999)
            spinbox.setToolTip(f"rango: {minimo} - {maximo} {unidad}")
            spinbox.setButtonSymbols(QSpinBox.NoButtons)
            spinbox.editingFinished.connect(lambda n=nombre: self.aplicar_parametro(n))
            form.addRow(f"{nombre} ({unidad})" if unidad else nombre, spinbox)
            self.spinboxes[nombre] = spinbox
        panel.addLayout(form)
        panel.addSpacing(15)
        self.agregar_boton(panel, "Start", self.iniciar)
        for cmd in ("pause", "resume"):
            self.agregar_boton(panel, cmd.capitalize(), lambda c=cmd: self.comando(c))
        self.agregar_boton(panel, "Cancel", self.cancelar)
        panel.addSpacing(15)

        self.lbl_recibidos = QLabel("")  # vacia hasta que llega el primer SWEEP
        self.lbl_respuesta = QLabel("")  # solo muestra errores
        self.lbl_respuesta.setStyleSheet("color: red")
        self.lbl_respuesta.setWordWrap(True)
        panel.addWidget(self.lbl_recibidos)
        panel.addSpacing(15)
        panel.addWidget(self.lbl_respuesta)
        panel.addStretch()

        lateral = QWidget()
        lateral.setLayout(panel)
        lateral.setFixedWidth(240)

        layout = QHBoxLayout()
        layout.addWidget(self.grafico, stretch=1)
        layout.addWidget(lateral)
        central = QWidget()
        central.setLayout(layout)
        self.setCentralWidget(central)

    @staticmethod
    def agregar_boton(panel, texto, accion):
        boton = QPushButton(texto)
        boton.clicked.connect(lambda: accion())
        panel.addWidget(boton)

    # ---------- comandos ----------

    def comando(self, linea):
        """Manda el comando. Si falla muestra el error en el panel; si sale
        bien se borra el error anterior."""
        resp = enviar_comando(linea)
        error = resp.startswith(("ERR", "sin respuesta", "write"))
        # ERR 5 solo se avisa en start; en pause/resume/cancel
        # no hace falta, el barrido sigue como estaba
        if resp.startswith("ERR 5") and linea in ("pause", "resume", "cancel"):
            error = False
        self.lbl_respuesta.setText(f"{linea}: {resp}" if error else "")
        return resp

    def cargar_valores(self, resp):
        """Pone en los spinbox los valores de una respuesta de get o de un
        SWEEP. Son valores del ESP32, asi que dejan de estar invalidos."""
        for nombre, valor in campos(resp).items():
            if nombre in self.spinboxes and valor.isdigit():
                self.spinboxes[nombre].setValue(int(valor))
                self.invalidos.discard(nombre)

    def iniciar(self):
        """start, salvo que haya un valor mal puesto."""
        if self.invalidos:
            nombre = sorted(self.invalidos)[0]
            minimo, maximo, unidad = parametros[nombre]
            self.lbl_respuesta.setText(f"{nombre} {minimo} - {maximo} {unidad}")
            return
        self.comando("start")

    def cancelar(self):
        """cancel y limpia el grafico. Con puntos = 0, procesar_datos ignora
        los POINT que todavia esten en camino hasta el proximo SWEEP."""
        self.comando("cancel")
        barrido.puntos = 0
        barrido.pts = []
        self.curva.setData([], [])
        self.grafico.setYRange(DB_MIN_INICIAL, 0)
        self.lbl_recibidos.setText("")

    def leer_config(self):
        """get config y carga los valores en los spinbox (al iniciar)."""
        self.cargar_valores(self.comando("get config"))
        self.eje_frecuencia(self.spinboxes["frec_inicio"].value(), self.spinboxes["frec_final"].value())

    def aplicar_parametro(self, nombre):
        """set del parametro editado."""
        minimo, maximo, unidad = parametros[nombre]
        valor = self.spinboxes[nombre].value()
        if not minimo <= valor <= maximo:
            self.invalidos.add(nombre)
            self.lbl_respuesta.setText(f"{nombre} {minimo} - {maximo} {unidad}")
        elif self.comando(f"set {nombre} {valor}") != "OK":
            self.invalidos.add(nombre)
        else:
            self.invalidos.discard(nombre)

    def eje_frecuencia(self, frec_inicio, frec_final):
        """Eje X logaritmico cortado justo en frec_inicio y frec_final."""
        if frec_inicio < frec_final:
            self.grafico.setXRange(math.log10(frec_inicio), math.log10(frec_final), padding=0)

    # ---------- datos ----------

    def nueva_linea(self, linea):
        completo = procesar_datos(linea)
        if linea.startswith("SWEEP"):  # barrido nuevo: eje X y spinbox toman su config
            self.eje_frecuencia(barrido.frec_inicio, barrido.frec_final)
            self.cargar_valores(linea)

        dbs = [db for _, db in barrido.pts]
        frecs = [f for f, _ in barrido.pts]
        self.curva.setData(frecs, dbs)
        # Eje de dB: se agranda si llegan valores fuera de ese rango
        self.grafico.setYRange(min(dbs + [DB_MIN_INICIAL]), max(dbs + [0]))
        self.lbl_recibidos.setText(f"recibidos: {len(barrido.pts)}/{barrido.puntos}" + (" (completo)" if completo else ""))

# ===================== main =====================


def main():
    global fd_cmd, fd_data

    try:
        fd_cmd = os.open(ESP32_DEV_CMD, os.O_RDWR)
        fd_data = os.open(ESP32_DEV_DATA, os.O_RDONLY)
    except OSError as e:
        print(f"open: {e}")
        return 1

    # Descarta la basura que haya quedado en cmd por arranque del ESP32 o mensajes anteriores
    limpiar_cmd(TIMEOUT_LIMPIEZA_MS)

    app = QApplication(sys.argv)
    ventana = Ventana()
    monitor.nueva_linea.connect(ventana.nueva_linea)
    # daemon: el hilo termina solo al cerrar la app
    threading.Thread(target=hilo_monitor, daemon=True).start()

    ventana.show()
    ventana.leer_config()  # arranca con la configuracion actual del ESP32
    ret = app.exec()

    os.close(fd_data)
    os.close(fd_cmd)
    return ret


if __name__ == "__main__":
    sys.exit(main())
