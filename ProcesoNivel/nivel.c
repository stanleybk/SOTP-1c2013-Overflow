/*
 * nivel.c
 *
 *  Created on: 29/04/2013
 *      Author: utnso
 */
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <curses.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <socketsOv.h>
#include <pthread.h>
#include "tad_items.h" //aca ya se incluye el #include "nivel.h"
#include "collections/list.h"

static int rows,cols;
int socketOrq;
ITEM_NIVEL * listaItems = NULL;
t_list * listaPersonajes;
t_log * logger;
int* deadlockActivado;
int* sleepDeadlock;
int recovery=0;
typedef struct t_posicion {
	int8_t x;
	int8_t y;
} Posicion;

//todo borrar
//typedef struct t_datap {
//int socket;
//ITEM_NIVEL * nodo;
//} DataP;

typedef struct t_nodoPers {
int socket;
ITEM_NIVEL * nodo;
char id; //todo redundante, checkear si se puede sacar y usar la info del nodo
t_list * listaRecursosAsignados;//lista de nodos recursos
char recBloqueado;
int personajeBloqueado;
} NodoPersonaje;

typedef struct t_nodoRec {
char id;
int cantAsignada;
} NodoRecurso;



void handler(NodoPersonaje* dataPer);
void listenear(int socketEscucha);
void sacarInfoCaja(char * caja, char* id, int* x , int* y, int* cant);
t_list* inicializarListaRecursos(void);
void liberarRecursos(t_list* listaPer,char personaje);
void desconexion(NodoPersonaje* dataPer);
void detectarDeadlock(void);
//1) El proceso nivel crea 1 lista (global)
//que tiene personajes e items

//2) Listenea personajes para conectarse
//cuando conectan:
//-Crea el thread correspondiente
//-Le pide el simbolo que usa
//-Con el simbolo, lo agrega a la lista de personajes

//3) El personaje le pide proximo recurso
//Nivel busca en la lista de recursos
//y le pasa la posicion del recurso

//4) El personaje avisa a nivel que se mueve
//Nivel va actualizando la posicion
//y chequea que este dentro de los margenes (rows,cols)

//5) El personaje esta en el recurso y solicita instancia
//Verifica que el personaje este en ese lugar
//Nivel busca en la lista de recursos si tiene
//instancia de ese recurso. Si la tiene, se la da
//y actualiza las instancias disponibles de ese recurso
//si no tiene, le avisa que no tiene

//6) El personaje se desconecta (por muerte o porque completa nivel)
//Nivel notifica a Orquestador instancias a liberar
//Nivel libera instancias que tenia el personaje
//Lo saca de la lista

//7) Chequeo de interbloqueo
//thread adicional que cada cierto tiempo (configurable)
//chequee que un personaje este bloqueado (lo veremos despues esto)
int main(void){
	//	Leer el config y cargar los recursos
	//Crea archivo log
	//Si existe lo abre, sino, lo crea
	//Con trace va a poder loguear t-o-d-o
	t_log_level detail = LOG_LEVEL_TRACE;

	//LogNivel = Nombre de archivo log
	//ProcesoNivel = Nombre del proceso
	//false = Que no aparezca en pantalla los logs
	//detail = Detalle con el que se va a loguear (TRACE,INFO,DEBUG,etc)
	logger = log_create("LogNivel.log","ProcesoNivel",false,detail);
	log_info(logger,"Se inicia el Nivel");
	t_config* configNivel = config_create("config.txt");
	char *varstr;
	varstr=malloc(sizeof(char)*8);
	strcpy(varstr,"Caja");
	char pal[4];
	int cantKeys = config_keys_amount(configNivel)-4;
	int x;
	for (x=1;x<=cantKeys;x++)
		{
			sprintf(pal, "%d", x);
			strcpy(varstr+4,pal);
			if(config_has_property(configNivel,varstr))
			{
				char *stringCompleto = config_get_string_value(configNivel,varstr);
				char id;
				int posx, posy, instancias;
				sacarInfoCaja(stringCompleto, &id, &posx, &posy, &instancias);
				CrearCaja(&listaItems, id, posx, posy, instancias);
			}
		}

	char* aux1;
	char** ipPuertoOrq;
	char* nombreNivel;
	int puertoEscuchaNivel=0;
	listaPersonajes= list_create();
	nombreNivel=config_get_string_value(configNivel,"Nombre");
	aux1=config_get_string_value(configNivel,"orquestador");
	recovery=config_get_int_value(configNivel,"Recovery");
	ipPuertoOrq=string_split(aux1, ":");
	int puertoOrq=(int)strtol(ipPuertoOrq[1], NULL, 10);

	socketOrq= quieroUnPutoSocketAndando(ipPuertoOrq[0],puertoOrq);

	//handshake Orquestador-Nivel
	if (mandarMensaje(socketOrq,2,strlen(nombreNivel)+1,nombreNivel)) {
		if(recibirMensaje(socketOrq,(void**)&aux1)>=0) {
			log_info(logger,"Conectado al Orquestador. Handshake %c Recibido",*aux1);
		}
	}
	Header unHeader;
	int* puerto;
	puerto=malloc(sizeof(int));
	if(recibirHeader(socketOrq,&unHeader)){

		if(recibirData(socketOrq,unHeader,(void**)puerto)){
//			printf("Se recibio el puerto\n");
			puertoEscuchaNivel=*puerto;
			mandarMensaje(socketOrq,1,sizeof(int),puerto);
		}
	}
	//*****  INICIO HEADER DEL DEADLOCK Y SLEEP ******
	//todo preguntar al ayudante de donde deberia venir esta info: si de la config del nivel o de la plataforma
		Header headerDeadlock;

		deadlockActivado=malloc(sizeof(int));
		if(recibirHeader(socketOrq,&headerDeadlock)){
//			printf("Se recibe validacion del Deadlock\n");
			if(recibirData(socketOrq,headerDeadlock,(void**)deadlockActivado)){
				mandarMensaje(socketOrq,1,sizeof(int),deadlockActivado);
			}
		}
		Header headerSleepDeadlock;

			sleepDeadlock=malloc(sizeof(int));
			if(recibirHeader(socketOrq,&headerSleepDeadlock)){
//				printf("Se recibio el Header sleep del Deadlock\n");
				if(recibirData(socketOrq,headerSleepDeadlock,(void**)sleepDeadlock)){
					mandarMensaje(socketOrq,1,sizeof(int),sleepDeadlock);
				}
			}
			log_debug(logger,"Vars recibidas: deadlock: %d sleepDL: %d",*deadlockActivado,*sleepDeadlock);
		//*****  FIN DEL HEADER DEL DEADLOCK Y SLEEP ******
		//********** Thread deteccion deadlock***********
			pthread_t thr_detectorDeadlock;
			pthread_create(&thr_detectorDeadlock, NULL, detectarDeadlock, NULL);
		//********** Fin Thread deteccion deadlock***********

	nivel_gui_inicializar();
	nivel_gui_get_area_nivel(&rows, &cols);
	nivel_gui_dibujar(listaItems);
//listenear

//	pthread_t threadListener;
//	pthread_create(&threadListener, NULL, listenear, NULL);

	int dameMaximo (t_list *lista)
	{
		int ii;
		int max;
		NodoPersonaje* nP;
		if (list_is_empty(lista))
			return 0;
		nP=list_get(lista,0);
		max = nP->socket;
		for (ii=0; ii<list_size(lista); ii++){
			nP=list_get(lista,ii);
			if (nP->socket > max)
				max = nP->socket;
		}
		return max;
	}

	fd_set descriptoresLectura;	/* Descriptores de interes para select() */						/* Buffer para leer de los socket */
	int maximo;							/* Número de descriptor más grande */
	int i;								/* Para bucles */

	int socketEscucha;
	socketEscucha=quieroUnPutoSocketDeEscucha(puertoEscuchaNivel);
	log_info(logger,"Escuchando Conexiones en el puerto: %d",puertoEscuchaNivel);
		while(1){
			NodoPersonaje * nodoP;
			/* Se inicializa descriptoresLectura */
			FD_ZERO (&descriptoresLectura);
			/* Se añade para select() el socket servidor */
			FD_SET (socketEscucha, &descriptoresLectura);
			/* Se añaden para select() los sockets con los clientes ya conectados */
			for (i=0; i<list_size(listaPersonajes); i++){
				nodoP=list_get(listaPersonajes,i);
				FD_SET(nodoP->socket, &descriptoresLectura);
			}
			/* Se el valor del descriptor más grande. Si no hay ningún cliente,
			 * devolverá 0 */
			maximo = dameMaximo (listaPersonajes);
			if (maximo < socketEscucha)
						maximo = socketEscucha;
			/* Espera indefinida hasta que alguno de los descriptores tenga algo
			 * que decir: un nuevo cliente o un cliente ya conectado que envía un
			 * mensaje */
			select (maximo + 1, &descriptoresLectura, NULL, NULL, NULL);
			/* Se comprueba si algún cliente ya conectado ha enviado algo */
			for (i=0; i<list_size(listaPersonajes); i++){
				nodoP=list_get(listaPersonajes,i);
				if (FD_ISSET (nodoP->socket, &descriptoresLectura))
				{
//					mandar info al handler de personaje
					handler(nodoP);
				}
			}

			/* Se comprueba si algún cliente nuevo desea conectarse y se le
			 * admite */
			if (FD_ISSET (socketEscucha, &descriptoresLectura))
			{
				//llamar al nuevaconexion
				listenear(socketEscucha);
			}



			nivel_gui_dibujar(listaItems);

		}

		usleep(2000000);
nivel_gui_terminar();
return 0;
}

//handler de cada personaje recibe un struct con el socket y el puntero a su nodo
void handler(NodoPersonaje* dataPer){
	void* buffer;
	int resp;
	Posicion posAux;
	char* carAux;
	carAux=malloc(sizeof(char));
	Header unHeader;
	ITEM_NIVEL* nodoAux;
	log_info(logger,"Personaje %c mando un mensaje", dataPer->nodo->id);

	//while(1){
		char idPer;
		char idRec;
		bool esPersonaje(NodoPersonaje* nodo){
						if(nodo->id==idPer)
							return true;
						return false;
					}
		bool esRecurso(NodoRecurso* nodo){
			if(nodo->id==idRec)
				return true;
			return false;
		}

		// todos los returns son para handlear el tema de desconexion del cliente sin aviso
		//todo hacer una funcion local q se encargue de tratar desconexion(cerrar el socket,borrar nodoPersonaje de listapersonaje y borrar personaje de la lista de items)
		if(recibirHeader(dataPer->socket,&unHeader) < 1)
		{
			desconexion(dataPer);
			//return;
		}
		else
		{
			log_debug(logger,"Llego Msj tipo %d payload %d", unHeader.type,unHeader.payloadlength);

					switch(unHeader.type){
						case 0:
							if(!recibirData(dataPer->socket,unHeader, (void**)carAux)) {close(dataPer->socket);return;}
							log_info(logger,"Llego el Personaje %c al nivel(Thread)", *carAux);
							if (mandarMensaje(dataPer->socket,0 , sizeof(char),carAux))
							{
								log_info(logger,"Se contesto el handshake");
							//}else {close(dataPer->socket);return;}
							}else {desconexion(dataPer);}
							break;
						case 1:
							resp=recibirData(dataPer->socket,unHeader, (void**)carAux);
							if(!resp) {close(dataPer->socket);return;}
							if(dataPer->personajeBloqueado){
								dataPer->personajeBloqueado=0;
								idPer=dataPer->nodo->id;
								NodoPersonaje* nodoPerAux = list_find(listaPersonajes,esPersonaje);
								idRec=dataPer->recBloqueado;
								NodoRecurso* nodoRecAux = list_find(nodoPerAux->listaRecursosAsignados,esRecurso);
								nodoRecAux->cantAsignada++;
//								restarRecurso(listaItems, dataPer->recBloqueado);
								log_debug(logger,"Hay instancias del recurso %c y se le dio una al Personaje %c",*carAux,dataPer->nodo->id);
							}
							log_debug(logger,"Resp del recData: %d",resp);
							log_info(logger,"El Personaje %c solicita la Posición del Recurso %c",dataPer->nodo->id,*carAux);
							nodoAux=obtenerRecurso(listaItems, *carAux);
							posAux.x=nodoAux->posx;
							posAux.y=nodoAux->posy;
							buffer=&posAux;
							if(mandarMensaje(dataPer->socket,1 , sizeof(Posicion),buffer)){
								log_info(logger,"Se mando la pos(%d,%d) del Rec %c al Personaje %c",posAux.x,posAux.y,*carAux,dataPer->nodo->id);
							//}else {close(dataPer->socket);return;}
							}else {desconexion(dataPer);}
							break;
						case 2:
							if(!recibirData(dataPer->socket, unHeader, (void**)&posAux)) {close(dataPer->socket);return;}
							if(dataPer->personajeBloqueado){
								dataPer->personajeBloqueado=0;
								idPer=dataPer->nodo->id;
								NodoPersonaje* nodoPerAux = list_find(listaPersonajes,esPersonaje);
								idRec=dataPer->recBloqueado;
								NodoRecurso* nodoRecAux = list_find(nodoPerAux->listaRecursosAsignados,esRecurso);
								nodoRecAux->cantAsignada++;
								log_debug(logger,"Hay instancias del recurso %c y se le dio una al Personaje %c",*carAux,dataPer->nodo->id);
//								restarRecurso(listaItems, dataPer->recBloqueado);
							}
							dataPer->nodo->posx=posAux.x;
							dataPer->nodo->posy=posAux.y;
							log_info(logger,"Se recibio la posicion(%d,%d) del Personaje %c",posAux.x,posAux.y,dataPer->nodo->id);
							carAux=malloc(sizeof(char));
							carAux[0]='K';
							if (mandarMensaje(dataPer->socket,4 , sizeof(char),(void*)carAux)) {
								log_info(logger,"Se le aviso al Personaje %c que llego bien su Posición",dataPer->nodo->id);
							//}else {close(dataPer->socket);return;}
							}else {desconexion(dataPer);}
							break;
						case 3:
							//Personaje solicita recurso
							if(!recibirData(dataPer->socket, unHeader, (void**)carAux)) {close(dataPer->socket);return;}
							log_info(logger,"El Personaje %c solicita una Instancia del Recurso %c",dataPer->nodo->id,*carAux);
							if(restarRecurso(listaItems, *carAux)>0)
							{
								idPer=dataPer->nodo->id;
								NodoPersonaje* nodoPerAux = list_find(listaPersonajes,esPersonaje);
								idRec=*carAux;
								NodoRecurso* nodoRecAux = list_find(nodoPerAux->listaRecursosAsignados,esRecurso);
								nodoRecAux->cantAsignada++;
								log_info(logger,"Hay instancias del recurso %c y se le dio una al Personaje %c",*carAux,dataPer->nodo->id);
					//			nodoAux=obtenerRecurso(listaItems, *carAux);
								int* respRec;
								respRec=malloc(sizeof(int));
								*respRec=1;
								if (mandarMensaje(dataPer->socket,4 , sizeof(int),(void*)respRec)) {
									log_info(logger,"Se mando la confirmacion %d del pedido de Recurso",*respRec);
								//}else {close(dataPer->socket);return;}
								}else {desconexion(dataPer);}
								free(respRec);

							}else{
								idPer=dataPer->nodo->id;
								NodoPersonaje* nodoPerAux = list_find(listaPersonajes,esPersonaje);
								nodoPerAux->personajeBloqueado=1;
								nodoPerAux->recBloqueado=*carAux;
								log_info(logger,"No hay instancias del recurso %c y no se le dio una al Personaje %c",*carAux,dataPer->nodo->id);
								int*respRec;
								respRec=malloc(sizeof(int));
								*respRec=0;
								if (mandarMensaje(dataPer->socket,4 , sizeof(int),(void*)respRec)) {
									log_info(logger,"Se mando la confirmacion %d del pedido de Recurso",*respRec);
								//} else {close(dataPer->socket);return;}
								}else {desconexion(dataPer);}
								free(respRec);
							}
							break;
						case 4:
							if(!recibirData(dataPer->socket, unHeader, (void**)carAux)) {close(dataPer->socket);return;}
							//todo sincronizar con los demas threads y con el listener cuando crea un personaje nuevo!
							if(dataPer->personajeBloqueado&&dataPer->nodo->id==*carAux){
								dataPer->personajeBloqueado=0;
								idPer=dataPer->nodo->id;
								NodoPersonaje* nodoPerAux = list_find(listaPersonajes,esPersonaje);
								idRec=dataPer->recBloqueado;
								NodoRecurso* nodoRecAux = list_find(nodoPerAux->listaRecursosAsignados,esRecurso);
								nodoRecAux->cantAsignada++;
//								restarRecurso(listaItems, dataPer->recBloqueado);
								log_debug(logger,"Hay instancias del recurso %c y se le dio una al Personaje %c",dataPer->recBloqueado,dataPer->nodo->id);
							}
							log_info(logger,"El Personaje %c solicita salir del nivel.",(dataPer->nodo)->id);
							log_debug(logger,"Case 4 puntero del personaje: %p",dataPer->nodo);
							liberarRecursos(listaPersonajes,(dataPer->nodo)->id);
							BorrarItem(&listaItems,(dataPer->nodo)->id);
							log_info(logger,"El personaje salio del Nivel.");
							close(dataPer->socket);
							log_debug(logger,"Se cerro el Socket: %d",dataPer->socket);
							//return;
							break;
						default:
								break;
					}
		}

//		free(carAux);
	//}

}


void listenear(int socketEscucha){
//todo borrar
//	listaPersonajes= list_create();
//

//    socketEscucha=quieroUnPutoSocketDeEscucha(puertoEscuchaGlobal);
//
//            while (1){

	int socketNuevaConexion;
		// Escuchar nuevas conexiones entrantes.
		if (listen(socketEscucha, 1) != 0) {

			log_error(logger,"Error al bindear socket escucha");
			//return EXIT_FAILURE;
		}

			log_info(logger,"Escuchando conexiones entrantes");

			// Aceptar una nueva conexion entrante. Se genera un nuevo socket con la nueva conexion.
			// La funcion accept es bloqueante, no sigue la ejecuci贸n hasta que se reciba algo
			if ((socketNuevaConexion = accept(socketEscucha, NULL, 0)) < 0) {

				log_error(logger,"Error al aceptar conexion entrante");
				//return EXIT_FAILURE;
			}

			//Handshake en el que recibe la letra del personaje
			char* per;
			if(recibirMensaje(socketNuevaConexion, (void**)&per)>=0) {
				log_info(logger,"Llego el Personaje %c del nivel",*per);
				if (mandarMensaje(socketNuevaConexion,0 , sizeof(char),per)) {
					log_info(logger,"Mando mensaje al personaje %c",*per);
				}

				NodoPersonaje* nodoPer;
				nodoPer = malloc(sizeof(NodoPersonaje));
				nodoPer->id=*per;
				nodoPer->socket=socketNuevaConexion;
				nodoPer->nodo = CrearPersonaje(&listaItems, *per, 0 ,0);
				nodoPer->personajeBloqueado=0;
				nodoPer->recBloqueado='0';
				nodoPer->listaRecursosAsignados=inicializarListaRecursos();
				list_add(listaPersonajes,nodoPer);

//				todo borrar DataP* personaje;
//				personaje=malloc(sizeof(DataP));
//				personaje->socket = socketNuevaConexion;
//				//Agrega personaje a la lista y devuelve nodo
//				personaje->nodo = CrearPersonaje(&listaItems, *per, 0 ,1);
//				log_info(logger,"Mando el socket %d (Thread)", personaje->socket);
				//TODO Fede vos sabes que hacer
//				pthread_t threadPersonaje;
//
//				pthread_create(&threadPersonaje, NULL, handler, (void *)personaje);

			}
//return 1;
}

void sacarInfoCaja(char * caja, char* id, int* x , int* y, int* cant)
{
	char ** vecStr;
	char* aux;
	vecStr=string_split(caja, ",");
	*id=*vecStr[1];
	*x=(int)strtol(vecStr[4], &aux, 10);
	*y=(int)strtol(vecStr[3], &aux, 10);
	*cant=(int)strtol(vecStr[2], &aux, 10);
	}
t_list* inicializarListaRecursos(void){
	t_list* lista;
	lista=list_create();
	NodoRecurso* nodoRec;
	ITEM_NIVEL * temp = listaItems;


//	if ((temp != NULL) && (temp->item_type == RECURSO_ITEM_TYPE)) {
//		nodoRec=malloc(sizeof(NodoRecurso));
//		nodoRec->cantAsignada=0;
//		nodoRec->id=temp->id;
//		list_add(lista,nodoRec);
//	}
		while(temp != NULL){
			while((temp != NULL) && (temp->item_type != RECURSO_ITEM_TYPE)) {
					temp = temp->next;
			}
			if ((temp != NULL) && (temp->item_type == RECURSO_ITEM_TYPE)) {
					nodoRec=malloc(sizeof(NodoRecurso));
					nodoRec->cantAsignada=0;
					nodoRec->id=temp->id;
					list_add(lista,nodoRec);

					temp = temp->next;
			}
		}

	return lista;
}
void liberarRecursos(t_list* listaPer,char personaje){
	char idPer;
	bool esPersonaje(NodoPersonaje* nodo){
					if(nodo->id==idPer)
						return true;
					return false;
				}
	void liberarInstancias(NodoRecurso* nodo){
		ITEM_NIVEL * auxItem=  obtenerRecurso(listaItems,nodo->id);
		log_debug(logger,"L BF Rec: %c Instancias: %d",nodo->id,auxItem->quantity);
		if(sumarRecurso(listaItems,nodo->id,nodo->cantAsignada)){
			log_info(logger,"Se libero %d instancias del Recurso %c",nodo->cantAsignada,nodo->id);
			log_debug(logger,"L AF Rec: %c Instancias: %d",nodo->id,auxItem->quantity);
			free(nodo);
		}
	}
	idPer=personaje;
	NodoPersonaje* nodoPerAux = list_remove_by_condition(listaPer,esPersonaje);

	NodoRecurso* buffer=malloc(sizeof(NodoRecurso)*list_size(nodoPerAux->listaRecursosAsignados));
	int i;
	for(i=0;i<list_size(nodoPerAux->listaRecursosAsignados);i++){
		memcpy(&(buffer[i]),list_get(nodoPerAux->listaRecursosAsignados,i),sizeof(NodoRecurso));
	}
	mandarMensaje(socketOrq,4,sizeof(NodoRecurso)*list_size(nodoPerAux->listaRecursosAsignados),(void*) buffer);

	//recibir recursos asignados
	Header unHeader;
	//todo validar
	recibirHeader(socketOrq,&unHeader);
	recibirData(socketOrq,unHeader,(void**)buffer);
	for(i=0;i<(list_size(nodoPerAux->listaRecursosAsignados));i++){
		log_debug(logger,"Se asigno %d instancias del Recurso %c segun el Orquestador",(buffer[i].cantAsignada),buffer[i].id);
		ITEM_NIVEL * auxItem=  obtenerRecurso(listaItems,buffer[i].id);
		log_debug(logger,"A BF Rec: %c Instancias: %d",buffer[i].id,auxItem->quantity);
		sumarRecurso(listaItems,buffer[i].id,-(buffer[i].cantAsignada));
		log_debug(logger,"A AF Rec: %c Instancias: %d",buffer[i].id,auxItem->quantity);}
	free(buffer);
	list_destroy_and_destroy_elements(nodoPerAux->listaRecursosAsignados,liberarInstancias);
	free(nodoPerAux);

}

//Maneja la desconexion de un PJ inesperadamente
//limpiando el nivel

void desconexion(NodoPersonaje* dataPer){

	log_info(logger,"El PJ %c se ha desconectado",dataPer->id);
	BorrarItem(&listaItems,(dataPer->nodo)->id);
	liberarRecursos(listaPersonajes,dataPer->id);
	close(dataPer->socket);
}

void detectarDeadlock(){

while(*deadlockActivado){
	int totalPj,totalRec,i,j;
	int** asignados;
	int** requeridos;
	int* disponible;
	int* requeridosTot;
	char* recursos;
	bool * finish;
	NodoPersonaje* nodoPer;
	NodoRecurso* nodoRec;
	ITEM_NIVEL* itemNivel;
	bool flag=true;
	log_info(logger,"Algoritmo de Deteccion de Deadlock empezado...");
	totalPj=list_size(listaPersonajes);
	int count=0;
	int countAnt=totalPj;
	if(totalPj==0) {
		log_info(logger,"Algoritmo de Deteccion de Deadlock suspendido por falta de personajes...");
		usleep(*sleepDeadlock);
		continue;
	}
	totalRec=cantidadItems(listaItems,RECURSO_ITEM_TYPE);
	log_debug(logger,"TotPj: %d Tot Recursos: %d count: %d countAnt: %d",totalPj,totalRec,count, countAnt);
	asignados=malloc(sizeof(int*) * totalPj);
	requeridos=malloc(sizeof(int*) * totalPj);
	disponible=malloc(sizeof(int)*totalRec);
	requeridosTot=malloc(sizeof(int)*totalRec);
	recursos=malloc(sizeof(char)*totalRec);
	finish=malloc(sizeof(bool)*totalPj);
	//inicializador de requeridos totales
	for(j=0;j<totalRec;j++)
		requeridosTot[j]=0;
	//armado de matriz de recursos asignado y matriz de flags finish

	for(i=0;i<totalPj;i++) {
		finish[i]=false;
		nodoPer=list_get(listaPersonajes,i);
		asignados[i]=malloc(sizeof(int)*totalRec);
		for(j=0;j<totalRec;j++) {

			nodoRec=list_get((nodoPer->listaRecursosAsignados),j);
			asignados[i][j]=nodoRec->cantAsignada;
		}
	}

	//armado de matriz de recursos solicitados
	for(i=0;i<totalPj;i++)    {
		nodoPer=list_get(listaPersonajes,i);
		requeridos[i]=malloc(sizeof(int)*totalRec);
		for(j=0;j<totalRec;j++) {
			requeridos[i][j]=0;

			if(nodoPer->personajeBloqueado){
				nodoRec=list_get((nodoPer->listaRecursosAsignados),j);
				if(nodoPer->recBloqueado==nodoRec->id){
					requeridos[i][j]=1;
					requeridosTot[j]+=1;
				}
			}
		}
	}
	//armado de vector de recursos disponibles
	for(j=0;j<totalRec;j++) {
		nodoRec=list_get((nodoPer->listaRecursosAsignados),j);
		itemNivel=obtenerRecurso(listaItems,nodoRec->id);
		recursos[j]=itemNivel->id;
		disponible[j]=itemNivel->quantity;
	}
	log_debug(logger,"BoolWhile: %d (count<countAnt): %d count: %d countAnt: %d flag: %d", ((count<countAnt)&&flag),(count<countAnt),count,countAnt,flag);
	int banderaAnt=0;
	while((count<countAnt)&&flag) {   //if finish array has all true's(all processes to running state)
		if(banderaAnt)countAnt=count;
		flag=false;				   //deadlock not detected and loop stops!
		for(i=0;i<totalPj;i++)    {
			count=0;
			nodoPer=list_get(listaPersonajes,i);
			//To check whether resources can be allocated any to blocked process
			if(finish[i]==false)
			{
				for(j=0;j<totalRec;j++) {
					log_debug(logger,"Recurso: %c RecAsig: %d RecReq: %d RecDisp: %d",recursos[j],asignados[i][j],requeridos[i][j],disponible[j]);
						if(requeridos[i][j]<=disponible[j])
						{
							count++;
						}
				}

				if(count==totalRec)
				{
					for(j=0;j<totalRec;j++){
						disponible[j]+=asignados[i][j];//allocated reources are released and added to available!
					}
					finish[i]=true;
					log_info(logger,"Personaje %c se pasa al estado ejecutando y se asume terminado",nodoPer->id);
				}
				else{
					flag=true;
					log_info(logger,"Personaje %c no se pasa al estado ejecutando y no se puede terminar",nodoPer->id);
				}
			}
		}
		count=0;
		for(i=0;i<totalPj;i++)    {
			if(finish[i]==false)
			{
				count++;
			}
		}
		log_debug(logger,"count: %d flag %d",count,flag);
		banderaAnt=1;
	}//fin while
	int suma,sumaBools;
	char* pjsEnDeadlock;
	Header unHeader;
	unHeader.type=3;
	unHeader.payloadlength=0;
	log_info(logger,"%d Personajes en DeadLock o starvation",count);
	pjsEnDeadlock=malloc(sizeof(char)*(totalPj-count));
	sumaBools=1;
	for(i=0;i<totalPj;i++){
		suma=0;
		sumaBools=sumaBools&&finish[i];
		if(finish[i]==false)
		{
			for(j=0;j<totalRec;j++){
				suma+=((asignados[i][j]>0)&&(asignados[i][j]<=requeridosTot[j]));
			}
			nodoPer=list_get(listaPersonajes,i);
			if(suma>0){
			pjsEnDeadlock[unHeader.payloadlength]=nodoPer->nodo->id;
			unHeader.payloadlength++;
			log_info(logger,"Shit! Deadlock detectado en el personaje: %c",nodoPer->id);
			}
			else
			log_info(logger,"Shit! Starvation detectado en el personaje: %c",nodoPer->id);
		}
	}
	if(sumaBools)
		log_info(logger,"Vamo'! Deadlock no Detectado");
	else if(recovery&&(unHeader.payloadlength>0)){//handlear deadlock
		unHeader.payloadlength=unHeader.payloadlength*sizeof(char);
		log_info(logger,"Se envio pedido de recovery al Orquestador. Se envio %d personajes en deadlock",unHeader.payloadlength);
		mandarMensaje(socketOrq,unHeader.type,unHeader.payloadlength,pjsEnDeadlock);
	}else log_info(logger,"Recovery desactivado %d",recovery);
	free(pjsEnDeadlock);
	for(i=0;i<totalPj;i++)    {
		free(asignados[i]);
		free(requeridos[i]);
	}
	free(asignados);
	free(requeridos);
	free(recursos);
	free(disponible);
	free(finish);
	log_debug(logger,"Sleep iniciado: %d",*sleepDeadlock);
	usleep(*sleepDeadlock);
	log_debug(logger,"Sleep terminado: %d",*sleepDeadlock);
	}
	return;
}
