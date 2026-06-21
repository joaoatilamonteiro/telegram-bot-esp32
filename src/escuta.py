import socket, os, threading
from getmac import get_mac_address

def captura_mac():
    mac = get_mac_address()
    return mac.upper()

HOST = '0.0.0.0'
PORT = 5000



# As chaves binárias exatas configuradas no ESP32
PING_BINARIO = "01110110 01101111 01100011 01100101 00100000 01100101 01110011 01110100 01100001 00100000 01100001 01110001 01110101 01101001 00111111 00001010"
RESPOSTA_BINARIA = "01100101 01110011 01110100 01101111 01110101 00100000 01100001 01110001 01110101 01101001 00001010"
DESLIGAR_BINARIO = "01000100 01000101 01010011 01001100 01001001 01000111 01000001 01010010 01011111 01010000 01000011"

def servidor_udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((HOST, PORT))

    while True:
        data, addr = s.recvfrom(1024)
        mensagem = data.decode("utf-8").strip()
        ip_esp = addr[0]

        print(ip_esp)

        if mensagem == PING_BINARIO:
            meu_mac = captura_mac()

            s.sendto(f"{RESPOSTA_BINARIA}, {meu_mac}".encode("utf-8"), addr)
            print(f"Ping em binário do ESP32 recebido! Autenticando com MAC: {meu_mac}")

        elif mensagem == DESLIGAR_BINARIO:
            print(f"Comando fatal recebido de {ip_esp}! Exibindo pop-up e desligando...")

            # A crase + n (`n) é a única diferença necessária
            title = "Encerrando a maquina"
            message = "Comando recebido! Desligando o sistema em 10 segundos..."

            os.system("shutdown /s /t 13")

            # A sua linha de comando original, perfeita e sem alterações:
            os.system(f'powershell -Command "Add-Type -AssemblyName PresentationFramework;[System.Windows.MessageBox]::Show(\'{message}\',\'{title}\')"')
            # 4. Desligamento real em 5 segundos


def servidor_tcp():
    tcp_s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_s.bind((HOST, PORT))
    tcp_s.listen(5)
    print(f"[TCP] Servidor a escutar na porta {PORT} (Rastreio de LED)")

    while True:
        # Fica a aguardar o ESP32 tentar conectar
        conn, addr = tcp_s.accept()
        conn.close()


if __name__ == "__main__":
    print("Iniciando Serviços de Escuta...")

    # Criamos as duas linhas de execução independentes
    thread_udp = threading.Thread(target=servidor_udp)
    thread_tcp = threading.Thread(target=servidor_tcp)

    # Damos o "Play" nas duas
    thread_udp.start()
    thread_tcp.start()