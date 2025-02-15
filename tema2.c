#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define TAG_SWARM_REQUEST 1
#define TAG_SWARM_RESPONSE 2
#define TAG_SEGMENT_REQUEST 3
#define TAG_SEGMENT_RESPONSE 4
#define TAG_FILE_COMPLETE 5
#define TAG_DOWNLOAD_COMPLETE 6
#define TAG_FINALIZE 7
#define TAG_UPLOAD_TERMINATE 8
#define TAG_LOAD_REQUEST 9
#define TAG_LOAD_RESPONSE 10
#define TAG_SEGMENT_AVAILABILITY_REQUEST 11
#define TAG_SEGMENT_AVAILABILITY_RESPONSE 12

typedef struct {
    int position;               // pozitia segmentului in fisier
    char hash[HASH_SIZE + 1];   // hash-ul segmentului
} Segment;

typedef struct {
    char name[MAX_FILENAME];    // numele fisierului
    int num_segments;           // nr total de segmente din fisier
    Segment segments[MAX_CHUNKS]; // array de segmente
} File;

typedef struct {
    int num_owned_files;        // nr de fisiere detinute de client
    File owned_files[MAX_FILES]; // array de fisiere deținute
    int num_desired_files;      // nr de fisiere dorite
    char desired_files[MAX_FILES][MAX_FILENAME]; // numele fisierelor dorite
    int num_downloaded_files;  // nr de fisiere descarcate partial
    File downloaded_files[MAX_FILES]; // array de fisiere descarcate partial
} ClientInfo;

typedef struct {
    char filename[MAX_FILENAME];    // numele fisierului
    int num_segments;               // nr de segmente
    char hashes[MAX_CHUNKS][HASH_SIZE + 1]; // hash-urile segmentelor
    int num_clients;                // nr de clienti care detin segmente
    int client_ranks[MAX_FILES];    // lista ranks ale clientilor (seeds/peers)
} FileSwarm;

typedef struct {
    int num_files;                  // nr total de fisiere
    int num_clients_finished; // nr de clienti care au terminat descarcarile
    FileSwarm files[MAX_FILES];     // swarm-urile pentru fiecare fisier
} TrackerInfo;

typedef struct {
    int rank;                     // rank-ul clientului
    ClientInfo *client_info;      // informatiile despre client
    pthread_mutex_t *load_mutex;  // mutex-ul utilizat pentru sincronizare
    int current_load;             // sarcina curentă (upload-uri si download-uri active)
} ThreadArgs;


void parse_input_file(const char *filename, ClientInfo *client_info) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        exit(EXIT_FAILURE);
    }

    client_info->num_downloaded_files = 0;

    // citeste numarul de fisiere detinute
    if (fscanf(file, "%d", &client_info->num_owned_files) != 1) {
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < client_info->num_owned_files; i++) {
        File *owned_file = &client_info->owned_files[i];

        if (fscanf(file, "%s %d", owned_file->name, &owned_file->num_segments) != 2) {
            exit(EXIT_FAILURE);
        }

        for (int j = 0; j < owned_file->num_segments; j++) {
            Segment *segment = &owned_file->segments[j];

            if (fscanf(file, "%s", segment->hash) != 1) {
                exit(EXIT_FAILURE);
            }
            segment->position = j;
        }
    }

    // citeste numarul de fisiere dorite
    if (fscanf(file, "%d", &client_info->num_desired_files) != 1) {
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < client_info->num_desired_files; i++) {
        if (fscanf(file, "%s", client_info->desired_files[i]) != 1) {
            exit(EXIT_FAILURE);
        }
    }

    fclose(file);
}

void initialize_downloaded_file(ClientInfo *client_info, const char *filename) {
    for (int i = 0; i < client_info->num_downloaded_files; i++) {
        if (strcmp(client_info->downloaded_files[i].name, filename) == 0) {
            return; // fisierul exista deja
        }
    }

    if (client_info->num_downloaded_files < MAX_FILES) {
        File *file = &client_info->downloaded_files[client_info->num_downloaded_files++];
        strcpy(file->name, filename);
        file->num_segments = 0;
    }
}

void add_segment_to_downloaded_file(ClientInfo *client_info, const char *filename, const char *hash, int position) {
    for (int i = 0; i < client_info->num_downloaded_files; i++) {
        if (strcmp(client_info->downloaded_files[i].name, filename) == 0) {
            File *file = &client_info->downloaded_files[i];

            // verifica daca segmentul exista deja
            for (int j = 0; j < file->num_segments; j++) {
                if (file->segments[j].position == position) {
                    return; // segmentul exista deja
                }
            }

            // adauga segmentul nou
            file->segments[file->num_segments].position = position;
            strcpy(file->segments[file->num_segments].hash, hash);
            file->num_segments++;

            return;
        }
    }
}

void add_client_to_swarm(FileSwarm *swarm, int client_rank) {
    for (int i = 0; i < swarm->num_clients; i++) {
        if (swarm->client_ranks[i] == client_rank) {
            return; // clientul este deja in swarm
        }
    }
    // adauga clientul in swarm
    swarm->client_ranks[swarm->num_clients++] = client_rank;
}

FileSwarm* add_file_to_tracker(TrackerInfo *tracker, const char *filename) {
    for (int i = 0; i < tracker->num_files; i++) {
        if (strcmp(tracker->files[i].filename, filename) == 0) {
            return &tracker->files[i]; // fisierul exista deja
        }
    }
    // creeaza un fisier nou
    FileSwarm *new_file = &tracker->files[tracker->num_files++];
    strcpy(new_file->filename, filename);
    new_file->num_clients = 0;
    return new_file;
}

void request_swarm_from_tracker(int rank, ClientInfo *client_info, FileSwarm *local_swarms, int *num_swarms) {
    MPI_Status status;

    for (int i = 0; i < client_info->num_desired_files; i++) {
        char *desired_file = client_info->desired_files[i];

        // trimite cererea pentru swarm-ul fisierului dorit catre tracker
        MPI_Send(desired_file, strlen(desired_file) + 1, MPI_CHAR, TRACKER_RANK, TAG_SWARM_REQUEST, MPI_COMM_WORLD);

        // Primeste swarm-ul fisierului de la tracker
        MPI_Recv(&local_swarms[*num_swarms], sizeof(FileSwarm), MPI_BYTE, TRACKER_RANK, TAG_SWARM_RESPONSE, MPI_COMM_WORLD, &status);

        // incrementeaza numarul de swarm-uri salvate
        (*num_swarms)++;
    }
}

void update_swarm(int rank, const char *filename, FileSwarm *swarm) {
    MPI_Status status;

    // trimite cererea catre tracker pentru swarm-ul fisierului specific
    MPI_Send(filename, strlen(filename) + 1, MPI_CHAR, TRACKER_RANK, TAG_SWARM_REQUEST, MPI_COMM_WORLD);

    // primeste swarm-ul actualizat de la tracker
    MPI_Recv(swarm, sizeof(FileSwarm), MPI_BYTE, TRACKER_RANK, TAG_SWARM_RESPONSE, MPI_COMM_WORLD, &status);

}

int is_segment_downloaded(File *file, int segment_position) {
    for (int i = 0; i < file->num_segments; i++) {
        if (file->segments[i].position == segment_position) {
            return 1; // segment deja descarcat
        }
    }
    return 0; // segmentul nu este descarcat
}

void save_file_in_order(File *downloaded_file, int rank) {
    // creeaza un fisier de iesire
    char output_filename[100];
    snprintf(output_filename, sizeof(output_filename), "client%d_%s", rank, downloaded_file->name);
    FILE *output_file = fopen(output_filename, "w");
    if (!output_file) {
        return;
    }

    // sorteaza segmentele in ordine dupa pozitie
    for (int i = 0; i < downloaded_file->num_segments - 1; i++) {
        for (int j = i + 1; j < downloaded_file->num_segments; j++) {
            if (downloaded_file->segments[i].position > downloaded_file->segments[j].position) {
                Segment temp = downloaded_file->segments[i];
                downloaded_file->segments[i] = downloaded_file->segments[j];
                downloaded_file->segments[j] = temp;
            }
        }
    }

    // scrie hash-urile in fisier
    for (int i = 0; i < downloaded_file->num_segments; i++) {
        fprintf(output_file, "%s\n", downloaded_file->segments[i].hash);
    }
    fclose(output_file);
}

void *download_thread_func(void *arg) {
    // arg transmise catre thread
    ThreadArgs *thread_args = (ThreadArgs *)arg;
    int rank = thread_args->rank;
    ClientInfo *client_info = thread_args->client_info;
    pthread_mutex_t *load_mutex = thread_args->load_mutex;
    int current_load = thread_args->current_load;

    // array pentru swarm-urile fisierelor dorite
    FileSwarm local_swarms[MAX_FILES];
    int num_swarms = 0;

    // daca nu exista fisiere dorite, se termina descarcarea
    // si se transmite mesaj la tracker
    if (client_info->num_desired_files == 0) {
        MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, TAG_DOWNLOAD_COMPLETE, MPI_COMM_WORLD);
        pthread_exit(NULL);
    }

    // solicita swarm-urile de la tracker
    request_swarm_from_tracker(rank, client_info, local_swarms, &num_swarms);

    // parcurge lista de fisiere dorite pentru a incepe descarcarea
    for (int i = 0; i < client_info->num_desired_files; i++) {
        char *desired_file = client_info->desired_files[i];

        // initializeaza fisierul descarcat
        initialize_downloaded_file(client_info, desired_file);
            
        // gaseste swarm-ul corespunzator
        FileSwarm *swarm = NULL;
        for (int j = 0; j < num_swarms; j++) {
            if (strcmp(local_swarms[j].filename, desired_file) == 0) {
                swarm = &local_swarms[j];
                break;
            }
        }

        if (!swarm) {
            continue;
        }

        // contor pentru segmentele descarcate pana la actualizare swarm
        int segments_downloaded = 0;

        // contor pentru segmentele descarcate in total
        int total_segments = 0;

        // parcurge segmentele fisierului
        for (int segment = 0; segment < swarm->num_segments; segment++) {
            
            // verifica daca segmentul a fost deja descarcat
            if (is_segment_downloaded(&client_info->downloaded_files[i], segment)) {
                total_segments++;
                continue;
            }

            char requested_hash[HASH_SIZE + 1];
            strcpy(requested_hash, swarm->hashes[segment]);

            int best_peer = -1; // cel mai bun peer pentru descarcarea segmentului
            int min_load = -1; // sarcina minima a peer-ului

            // parcurge lista de peers din swarm pentru a-l gasi pe cel mai optim
            for (int peer_index = 0; peer_index < swarm->num_clients; peer_index++) {
                int peer_rank = swarm->client_ranks[peer_index];
                
                if (peer_rank == rank) {
                    continue;
                }

                // verificam daca peer-ul are segmentul
                MPI_Send(requested_hash, strlen(requested_hash) + 1, MPI_CHAR, peer_rank, TAG_SEGMENT_AVAILABILITY_REQUEST, MPI_COMM_WORLD);
                char availability_response[50];
                MPI_Recv(availability_response, sizeof(availability_response), MPI_CHAR, peer_rank, TAG_SEGMENT_AVAILABILITY_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                if (strcmp(availability_response, "HAS_SEGMENT") == 0) {
                    MPI_Send(&rank, 1, MPI_INT, peer_rank, TAG_LOAD_REQUEST, MPI_COMM_WORLD);
                    int peer_load;
                    MPI_Recv(&peer_load, 1, MPI_INT, peer_rank, TAG_LOAD_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    // gaseste peer-ul cu sarcina minima
                    if (min_load == -1 || peer_load < min_load) {
                        min_load = peer_load;
                        best_peer = peer_rank;
                    }
                }
            }

            // daca s-a gasit un client optim, trimite cererea de descarcare
            if (best_peer != -1) {
                // blocam mutex-ul inainte de a incrementa contorul
                pthread_mutex_lock(load_mutex);
                current_load++;
                pthread_mutex_unlock(load_mutex);

                // trimite cererea de descarcare a segmentului
                MPI_Send(requested_hash, strlen(requested_hash) + 1, MPI_CHAR, best_peer, TAG_SEGMENT_REQUEST, MPI_COMM_WORLD);

                char response[50];
                MPI_Recv(response, sizeof(response), MPI_CHAR, best_peer, TAG_SEGMENT_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                if (strcmp(response, "ACK") == 0) {
                    add_segment_to_downloaded_file(client_info, desired_file, requested_hash, segment);
                    
                    segments_downloaded++;
                    total_segments++;

                    // blocam mutex-ul inainte de a decrementa contorul
                    pthread_mutex_lock(load_mutex);
                    current_load--;
                    pthread_mutex_unlock(load_mutex);

                    // dupa fiecare 10 segmente descarcate, actualizam swarm-ul
                    if (segments_downloaded == 10) {
                        update_swarm(rank, desired_file, swarm);
                        segments_downloaded = 0;
                    }

                } else {
                    // daca nu primim ACK, decrementam noi contorul
                    pthread_mutex_lock(load_mutex);
                    current_load--;
                    pthread_mutex_unlock(load_mutex);
                }
            }
        }

        File *downloaded_file = NULL;
        for (int k = 0; k < client_info->num_downloaded_files; k++) {
            if (strcmp(client_info->downloaded_files[k].name, desired_file) == 0) {
                downloaded_file = &client_info->downloaded_files[k];
                break;
            }
        }

        // salvam fisierul
        if (downloaded_file) {
            save_file_in_order(downloaded_file, rank);
                
        }

        // notifica tracker-ul ca fisierul a fost descarcat complet
        MPI_Send(desired_file, strlen(desired_file) + 1, MPI_CHAR, TRACKER_RANK, TAG_FILE_COMPLETE, MPI_COMM_WORLD);
    }

    // notifica tracker-ul ca toate descarcarile sunt complete
    MPI_Send(&rank, 1, MPI_INT, TRACKER_RANK, TAG_DOWNLOAD_COMPLETE, MPI_COMM_WORLD);

    pthread_exit(NULL);
    return NULL;
}

void tracker_handle_file_complete(TrackerInfo *tracker) {
    MPI_Status status;
    char completed_file[MAX_FILENAME];

    // primeste notificarea pentru un fisier complet descarcat
    MPI_Recv(completed_file, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, TAG_FILE_COMPLETE, MPI_COMM_WORLD, &status);

}

void tracker_handle_download_complete(TrackerInfo *tracker, int num_clients) {
    MPI_Status status;
    int client_rank;

    // primeste mesaj de la un client care indica faptul ca a terminat descarcarile
    MPI_Recv(&client_rank, 1, MPI_INT, MPI_ANY_SOURCE, TAG_DOWNLOAD_COMPLETE, MPI_COMM_WORLD, &status);
   
    // incrementeaza contorul de clienti care au terminat toate descarcarile
    tracker->num_clients_finished++;

    // daca toti clientii au terminat descarcarile
    if (tracker->num_clients_finished == num_clients - 1) {
        
        // trimite mesaj de finalizare catre toti clientii
        for (int i = 1; i < num_clients; i++) {
            int finalize_signal = 1;
            MPI_Send(&finalize_signal, 1, MPI_INT, i, TAG_FINALIZE, MPI_COMM_WORLD);
        }
    }
}

void *upload_thread_func(void *arg) {
    // argumentele transmise catre thread
    ThreadArgs *thread_args = (ThreadArgs *)arg;
    ClientInfo *client_info = thread_args->client_info;
    pthread_mutex_t *load_mutex = thread_args->load_mutex;
    int current_load = thread_args->current_load;

    while (1) {
        MPI_Status status;
        int flag;

        // verificam daca exista mesaje primite
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (flag) { // daca exista un mesaj
            switch (status.MPI_TAG) { // verifica tipul mesajului
                case TAG_UPLOAD_TERMINATE: {
                    // mesajul de terminare a procesului de upload
                    int terminate_signal;
                    MPI_Recv(&terminate_signal, 1, MPI_INT, MPI_ANY_SOURCE, TAG_UPLOAD_TERMINATE, MPI_COMM_WORLD, &status);
                    pthread_exit(NULL);
                    break;
                }

                case TAG_SEGMENT_REQUEST: {
                    // mesajul de cerere pentru un segment de fisier
                    char buffer[HASH_SIZE + 1];
                    MPI_Recv(buffer, sizeof(buffer), MPI_CHAR, status.MPI_SOURCE, TAG_SEGMENT_REQUEST, MPI_COMM_WORLD, &status);
                    int requesting_peer = status.MPI_SOURCE;

                    // cauta segmentul
                    int segment_found = 0;

                    // verificam in fisierele detinute integral
                    for (int i = 0; i < client_info->num_owned_files; i++) {
                        File *file = &client_info->owned_files[i];
                        for (int j = 0; j < file->num_segments; j++) {
                            if (strcmp(file->segments[j].hash, buffer) == 0) {
                                segment_found = 1;
                                break;
                            }
                        }
                        if (segment_found) break;
                    }

                    // verificam in fisierele descarcate
                    if (!segment_found) {
                        for (int i = 0; i < client_info->num_downloaded_files; i++) {
                            File *file = &client_info->downloaded_files[i];
                            for (int j = 0; j < file->num_segments; j++) {
                                if (strcmp(file->segments[j].hash, buffer) == 0) {
                                    segment_found = 1;
                                    break;
                                }
                            }
                            if (segment_found) break;
                        }
                    }

                    char response[50];
                    // segmentul este gasit
                    if (segment_found) {
                        strcpy(response, "ACK");
                        // blocam mutex-ul inainte de a incrementa contorul
                        pthread_mutex_lock(load_mutex);
                        current_load++;
                        pthread_mutex_unlock(load_mutex);
                        
                        // trimitem raspunsul
                        MPI_Send(response, strlen(response) + 1, MPI_CHAR, requesting_peer, TAG_SEGMENT_RESPONSE, MPI_COMM_WORLD);
                        
                        // decrementam contorul
                        pthread_mutex_lock(load_mutex);
                        current_load--;
                        pthread_mutex_unlock(load_mutex);
                        break;
                    } 
                }

                case TAG_SEGMENT_AVAILABILITY_REQUEST: {
                    // caz pentru cererea de disponibilitate segment
                    char buffer[HASH_SIZE + 1];
                    MPI_Recv(buffer, sizeof(buffer), MPI_CHAR, status.MPI_SOURCE, TAG_SEGMENT_AVAILABILITY_REQUEST, MPI_COMM_WORLD, &status);
                    int requesting_peer = status.MPI_SOURCE;

                    // cautam segmentul in fisierele detinute
                    int segment_found = 0;
                    for (int i = 0; i < client_info->num_owned_files; i++) {
                        File *file = &client_info->owned_files[i];
                        for (int j = 0; j < file->num_segments; j++) {
                            if (strcmp(file->segments[j].hash, buffer) == 0) {
                                segment_found = 1;
                                break;
                            }
                        }
                        if (segment_found) break;
                    }

                    // verificam in fisierele descarcate
                    if (!segment_found) {
                        for (int i = 0; i < client_info->num_downloaded_files; i++) {
                            File *file = &client_info->downloaded_files[i];
                            for (int j = 0; j < file->num_segments; j++) {
                                if (strcmp(file->segments[j].hash, buffer) == 0) {
                                    segment_found = 1;
                                    break;
                                }
                            }
                            if (segment_found) break;
                        }
                    }

                    char response[50];

                    if (segment_found) { // daca segmentul este gasit
                        strcpy(response, "HAS_SEGMENT");
                    } else { // daca nu
                        strcpy(response, "NO_SEGMENT");
                    }

                    // trimitem raspunsul
                    MPI_Send(response, strlen(response) + 1, MPI_CHAR, requesting_peer, TAG_SEGMENT_AVAILABILITY_RESPONSE, MPI_COMM_WORLD);
                    break;
                }

                case TAG_LOAD_REQUEST: {
                    // caz pentru cererea de load a unui peer
                    int requesting_client;
                    MPI_Recv(&requesting_client, 1, MPI_INT, status.MPI_SOURCE, TAG_LOAD_REQUEST, MPI_COMM_WORLD, &status);
                    MPI_Send(&current_load, 1, MPI_INT, requesting_client, TAG_LOAD_RESPONSE, MPI_COMM_WORLD);
                    break;
                }
                default:
                    break;
            }
        }
    }

    return NULL;
}

FileSwarm* find_swarm_by_filename(TrackerInfo *tracker, const char *filename) {
    // gasire swarm dupa numele fisierului
    for (int i = 0; i < tracker->num_files; i++) {
        if (strcmp(tracker->files[i].filename, filename) == 0) {
            return &tracker->files[i];
        }
    }
    return NULL;
}

void tracker_handle_swarm_request(TrackerInfo *tracker) {
    MPI_Status status;
    char requested_file[MAX_FILENAME];

    // primeste cererea pentru un fisier
    MPI_Recv(requested_file, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, TAG_SWARM_REQUEST, MPI_COMM_WORLD, &status);
    int client_rank = status.MPI_SOURCE;

    FileSwarm *swarm = find_swarm_by_filename(tracker, requested_file);
    if (swarm) {
        // trimite swarm-ul catre client
        MPI_Send(swarm, sizeof(FileSwarm), MPI_BYTE, client_rank, TAG_SWARM_RESPONSE, MPI_COMM_WORLD);
    
        // adauga clientul in swarm daca nu exista deja
        add_client_to_swarm(swarm, client_rank);
    }
}

void tracker_initialize(TrackerInfo *tracker, int numtasks) {

    int num_clients = numtasks - 1; // exclude trackerul
    int clients_received = 0;

    while (clients_received < num_clients) {
        MPI_Status status;
        ClientInfo client_info;

        // primeste informatiile de la un client
        MPI_Recv(&client_info, sizeof(ClientInfo), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        int client_rank = status.MPI_SOURCE;

        // procesam fisierele detinute de client
        for (int j = 0; j < client_info.num_owned_files; j++) {
            File *file = &client_info.owned_files[j];
            FileSwarm *swarm = add_file_to_tracker(tracker, file->name);

            // setam segmentele fisierului in swarm
            swarm->num_segments = file->num_segments;
            for (int k = 0; k < file->num_segments; k++) {
                strcpy(swarm->hashes[k], file->segments[k].hash);
            }

            // adaugam clientul ca seed
            add_client_to_swarm(swarm, client_rank);
        }

        // incrementam numarul de clienti care au trimis informatiile
        clients_received++;
    }

    // dupa ce toti clientii au trimis informatiile, trimitem ACK
    for (int i = 1; i < numtasks; i++) {
        char ack[] = "ACK";
        MPI_Send(ack, sizeof(ack), MPI_CHAR, i, 0, MPI_COMM_WORLD);
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void tracker(int numtasks, int rank) {
    // initializare trackerInfo
    TrackerInfo tracker_info = {0};

    // primeste informatiile de la clienti
    tracker_initialize(&tracker_info, numtasks);

    int should_finalize = 0;
    while (!should_finalize) {
        MPI_Status status;

        // verificam daca exista mesaje
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        if (status.MPI_TAG == TAG_SWARM_REQUEST) {
            // caz pentru cererea unui swarm
            tracker_handle_swarm_request(&tracker_info);
        } else if (status.MPI_TAG == TAG_FILE_COMPLETE) {
            // descarcarea unui file
            tracker_handle_file_complete(&tracker_info);
        } else if (status.MPI_TAG == TAG_DOWNLOAD_COMPLETE) {
            // un client care a descarcat toate fisierele
            tracker_handle_download_complete(&tracker_info, numtasks);

            // daca toti clientii au terminat descarcarile
            if (tracker_info.num_clients_finished == numtasks - 1) {
                should_finalize = 1; // trackerul se auto-finalizeaza
            }
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    ClientInfo client_info;
    char filename[200];
    snprintf(filename, sizeof(filename), "in%d.txt", rank);
    parse_input_file(filename, &client_info);

    // trimite informatiile despre fisierele detinute catre tracker
    MPI_Send(&client_info, sizeof(ClientInfo), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);

    char ack[4];
    MPI_Status status2;
    MPI_Recv(ack, sizeof(ack), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, &status2);
    if (strcmp(ack, "ACK") != 0) {
        exit(EXIT_FAILURE);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    pthread_mutex_t load_mutex = PTHREAD_MUTEX_INITIALIZER;
    int current_load = 0;
    // creeaza structura de argumente pentru thread-uri
    ThreadArgs args = {rank, &client_info, &load_mutex, current_load};

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *)&args);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&args);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    // asteapta semnalul de finalizare
    int finalize_signal;
    MPI_Status finalize_status;
    MPI_Recv(&finalize_signal, 1, MPI_INT, TRACKER_RANK, TAG_FINALIZE, MPI_COMM_WORLD, &finalize_status);

    // trimite mesaj de terminare catre thread-ul de upload
    int terminate_signal = 1;
    MPI_Send(&terminate_signal, 1, MPI_INT, rank, TAG_UPLOAD_TERMINATE, MPI_COMM_WORLD);

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la așteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la așteptarea thread-ului de upload\n");
        exit(-1);
    }

    // distrugem mutexul
    pthread_mutex_destroy(&load_mutex);
}

int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
