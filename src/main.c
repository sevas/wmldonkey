/*
   

   wmldonkey is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or                                                               
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License                                                                   
   for more details.                                                                                                                                                                                              
   You should have received a copy of the GNU General Public License                                                                          
   along with wmldonkey; see the file COPYING. If not, write to the                                                                             
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,                                                                               
   Boston, MA 02111-1307, USA. 
*/



#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define _GNU_SOURCE   // for getline()
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>

#include <X11/X.h>
#include <X11/xpm.h>
#include <X11/Xlib.h>

#include "wmgeneral.h"
#include "pixmaps.h"

#include <sys/types.h>
#include <signal.h>
#include <netdb.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>

// passwords issues
#include <termios.h>  // echoing is bad
#include <time.h>

#define PORT     4001  // mlgui port
#define SHMKEY1  'X'
#define SHMKEY2  'Y'


//Here we go
struct FileInfo
{
    int file_id;
    int file_size;
    int file_downloaded;
    unsigned char file_state;
    char down_rate[12];         // max 999999999.9 b/s, hope it'll do
    char pref_name[256];
};

struct Files
{
    int shmid1, shmid2;
    short int *nb;
    struct FileInfo *files1;
};

struct Page
{
    short int page_scroll;
    short int page;
    short int pos[4];
    short int scroll[4];
    int fileid[4];
//    short int sticky[4]; see TODO
};


struct Config
{
    char *hostname;
    char *rcfilename;
    char *login;
    char *password;
    int port;
    short int line_scroll;
    short int speed;
};

// global variables (sorry :o )
int g_sockfd, g_child_pid, g_child_is_dead = 0, g_verbose_mode = 0;
struct Files g_F;
char *g_trash = NULL;
char *g_pidfile = NULL;

// Prototypes  
static void print_usage (void);
static void ParseCMDLine (int argc, char *argv[], struct Config *, struct Page *);
void createDefaultCfg (struct Config *);
void readconf (struct Config *, struct Page *);
short int get_mldonkey_message ();
void init (struct sockaddr_in *, struct hostent *, struct Config, struct Page *, char **);
void draw_file_line (struct FileInfo, int, int, int);
void draw_file_percent (struct FileInfo, char *, int, int);
void draw_page (/*struct Files *, */struct Files *, struct Page *, struct Config);
void launch_child (int);
void filesCopy (struct Files *, struct Files *);
void make_password (struct Config);

// sighandlers
void disconnect (int);
void end_child (int);
void realloc_files1 (int);
void quit_proc (int);
void dummy (int);               // "do nothing" function (overwrite last sighandler | disclaimer : it sucks)
void child_died (int);
void int_child (int);
void reconnect(int);

//---

int main (int argc, char *argv[])
{
    XEvent event;

    struct hostent *he;
    struct sockaddr_in addr;
    struct Page P;
    struct Config conf;
    struct Files my_files;      // local copy of struct Files g_F (to avoid lags in displaying process)
    int region, i, page_change_cnt = 0;
    char *home;


    my_files.files1 = NULL;
    my_files.nb = malloc (sizeof (short int) * 3);

    conf.hostname = malloc (strlen ("localhost") + 1);
    strcpy (conf.hostname, "localhost");

    home = getenv ("HOME");

    conf.rcfilename = malloc (strlen (home) + strlen ("/.wmldonkeyrc") + 1);
    sprintf (conf.rcfilename, "%s/.wmldonkeyrc", home);
    conf.rcfilename[strlen (home) + strlen ("/.wmldonkeyrc")] = '\0';

    // default page's behavior
    P.page_scroll = 0;
    P.page = 0;
    for (i = 0; i < 4; i++)
    {
        P.scroll[i] = 1;
        P.pos[i] = 4;
    }

    readconf (&conf, &P);
    ParseCMDLine (argc, argv, &conf, &P);
    openXwindow (argc, argv, wmldonkey_master_xpm, xpm_mask_bits, xpm_mask_width, xpm_mask_height);

    init (&addr, he, conf, &P, argv);

    signal (SIGUSR1, realloc_files1);
    signal (SIGCHLD, child_died);
    signal (SIGSEGV, quit_proc);
    signal (SIGPIPE, reconnect);


    g_F.nb[2] = 1;                // lock
    launch_child (g_sockfd);


    signal (SIGINT, quit_proc);
    signal (SIGTERM, quit_proc);

    printf ("[main] syncing struct files\n");
    // sync g_F & my_files
    while (g_F.nb[2])
        sleep (1);
    filesCopy (&my_files, &g_F);

    if (g_verbose_mode)
    {
        printf ("[main] main proc: %d, child : %d\n", getpid (), g_child_pid);
        printf ("[main] entering main loop\n");
    }


    // Loop Forever 
    while (1)
    {
        if (g_child_is_dead)
        {
            g_child_is_dead = 0;
            if (g_verbose_mode)
                printf ("[main] child is dead, rebirth\n");
            launch_child (g_sockfd);
        }
        draw_page (&my_files, &P, conf);
        RedrawWindow ();

        // Process any pending X events.  
        while (XPending (display))
        {
            XNextEvent (display, &event);
            switch (event.type)
            {
                case Expose:
                    RedrawWindow ();
                    break;
                case ButtonPress:
                    break;
                case ButtonRelease:
                    region = CheckMouseRegion (event.xbutton.x, event.xbutton.y);

                    switch (region)
                    {
                            // page change
                        case 0:
                        case 1:
                            if (g_verbose_mode)
                                printf ("[main] Xevent : page change -> ");
                            // reset auto change counter first
                            page_change_cnt = 0;
                            if (event.xbutton.button == 1)  // left mouse button
                            {
                                if (g_verbose_mode)
                                    printf ("next -> ");

                                if (g_F.nb[0] <= 4)
                                {
                                    if (g_verbose_mode)
                                        printf ("no need to\n");

                                    P.page = 0;

                                    for (i = 0; i < 4; i++)
                                    {
                                        P.scroll[i] = conf.line_scroll;
                                        P.pos[i] = 4;
                                    }
                                }
                                else
                                {
                                    if (g_verbose_mode)
                                        printf ("proceeding\n");

                                    int p = (g_F.nb[0]);

                                    if (p % 4)  // if we have more than (k*4) files
                                        p = (p / 4) + 1;
                                    else
                                        p = p / 4;

                                    P.page = (P.page + 1) % /* (short int) */ p;

                                    for (i = 0; i < 4; i++)
                                    {
                                        P.scroll[i] = conf.line_scroll;
                                        P.pos[i] = 4;
                                    }
                                }
                            }
                            else if (event.xbutton.button == 3) // right mouse button
                            {
                                if (g_verbose_mode)
                                    printf ("back -> ");

                                if (g_F.nb[0] <= 4)
                                {
                                    if (g_verbose_mode)
                                        printf ("no need to\n");
                                    // P.page = 0;

                                    // for (i = 0; i < 4; i++)
                                    // {
                                    // P.scroll[i] = conf.line_scroll;
                                    // P.pos[i] = 4;
                                    // }
                                }

                                else
                                {
                                    if (g_verbose_mode)
                                        printf ("proceeding\n");

                                    int p = g_F.nb[0];

                                    // calculate how many pages we have
                                    if (p % 4)  // if we have more than (k*4) files
                                        p = (p / 4) + 1;
                                    else
                                        p = p / 4;

                                    // p=p/4;
                                    if (P.page == 0)
                                        P.page = p - 1;
                                    else
                                        P.page--;

                                    for (i = 0; i < 4; i++)
                                    {
                                        P.scroll[i] = conf.line_scroll;
                                        P.pos[i] = 4;
                                    }
                                }
                            }
                            if (g_verbose_mode)
                                printf ("[main] redrawing area\n");
                            // redraw empty page
                            copyXPMArea (100, 0, 56, 56, 4, 4);
                            RedrawWindow ();
                            break;

                            // stop scrolling 
                        case 2:
                        case 3:
                        case 4:
                        case 5:
                            if (g_verbose_mode)
                                printf ("[main] Xevent : ");
                            if (event.xbutton.button == 1)
                            {
                                if (g_verbose_mode)
                                    printf ("[main] pause/resume scrolling\n");
                                P.scroll[region - 2] = (P.scroll[region - 2] + 1) % 2;

                                P.pos[region - 2] = 4;
                            }
                            else if (event.xbutton.button == 3)
                            {

                                if (g_verbose_mode)
                                    printf ("[main] file to switch : %d, \n", P.fileid[region - 2]);
                                if (P.fileid[region - 2] != -1)
                                {
                                    for (i = 0; i < my_files.nb[0] && my_files.files1[i].file_id != P.fileid[region - 2]; i++)
                                        ;
                                    if (i < my_files.nb[0])
                                    {
                                        if (g_verbose_mode)
                                            printf ("[main] current status : %d\n", my_files.files1[i].file_state);
                                        int a;
                                        short int b, c;

                                        // send switchDownload (23)
                                        if (my_files.files1[i].file_state == 0 || my_files.files1[i].file_state == 1)
                                        {

                                            a = 7;
                                            b = 23;
                                            if (send (g_sockfd, &a, 4, 0) == -1)    // size
                                                perror ("send");
                                            if (send (g_sockfd, &b, 2, 0) == -1)    // opcode
                                                perror ("send");
                                            if (send (g_sockfd, &my_files.files1[i].file_id, 4, 0) == -1)   // file id
                                                perror ("send");
                                            if (send (g_sockfd, &my_files.files1[i].file_state, 1, 0) == -1)    // action
                                                perror ("send");
                                            if (g_verbose_mode)
                                                printf ("[main] switched\n");
                                        }
                                        // commit (13)
                                        else if (my_files.files1[i].file_state == 2)
                                        {
                                            c = strlen (my_files.files1[i].pref_name);
                                            a = 4 + 2 + 2 + c;
                                            b = 13;
                                            if (send (g_sockfd, &a, 4, 0) == -1)    // size
                                                perror ("send");
                                            if (send (g_sockfd, &b, 2, 0) == -1)    // opcode
                                                perror ("send");
                                            if (send (g_sockfd, &my_files.files1[i].file_id, 4, 0) == -1)   // file id
                                                perror ("send");
                                            if (send (g_sockfd, &c, 2, 0) == -1)    // str lgt
                                                perror ("send");
                                            if (send (g_sockfd, my_files.files1[i].pref_name, c, 0) == -1)  // string
                                                perror ("send");
                                            if (g_verbose_mode)
                                                printf ("[main] commited\n");
                                        }
                                    }
                                }
                                else
                                    printf ("\n");

                            }
                            break;  // end case 5

                        default:
                            break;
                    };

                    break;      // end case ButtonRelease
            }
        }
        if (P.page_scroll)
        {
            page_change_cnt += conf.speed * 10000;
            if (page_change_cnt >= P.page_scroll * 1000000)
            {
                if (g_verbose_mode)
                    printf ("[main] automatic page switch : ");

                // there's only one page -> do nothing
                if (my_files.nb[0] <= 4)
                {
                    if (g_verbose_mode)
                        printf ("less than 4 files, no need to\n");
                }
                else
                {
                    if (g_verbose_mode)
                        printf ("proceeding\n");
                    int p = (my_files.nb[0]);

                    if (p % 4)  // if we have more than (k*4) files
                        p = (p / 4) + 1;
                    else
                        p = p / 4;

                    P.page = (P.page + 1) % (short int) p;

                    for (i = 0; i < 4; i++)
                    {
                        P.scroll[i] = conf.line_scroll;
                        P.pos[i] = 4;
                    }

                }
                page_change_cnt = 0;

            }
        }
        usleep (conf.speed * 10000);

    }                           // end while(1)

// we should never get here 
    return 0;
}



//--------------------------------------
void init (struct sockaddr_in *addr, struct hostent *he,
           struct Config C, struct Page *P, char **argv)
{

    // splash screen (c)(r)(tm) 
    struct FileInfo splash;

    // this sucks :o, goto __LINE__ + 10 please :o
    splash.file_state = 0;
    strcpy (splash.down_rate, "0.0");
    strcpy (splash.pref_name, "connect");
    draw_file_line (splash, 58, 5, 5);
    strcpy (splash.pref_name, "ing to");
    draw_file_line (splash, 58, 5, 14);
    strcpy (splash.pref_name, "mlcore");
    draw_file_line (splash, 58, 5, 23);
    copyXPMArea (77, 50, 6, 6, 28, 50);
    RedrawWindow ();


    int i;

    he = gethostbyname (C.hostname);
    if (!he)
    {
        perror ("[init] gethost");
        exit (1);
    }

    // we need a tcp socket to talk with this thing
    if ((g_sockfd = socket (AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror ("[init] socket");
        exit (1);
    }
    if (g_verbose_mode)
        printf ("[init] socket created\n");
    addr->sin_family = AF_INET;
    addr->sin_port = htons (PORT);
    addr->sin_addr = *((struct in_addr *) (he->h_addr));
    memset (&(addr->sin_zero), '\0', 8);

    i = 0;
    int conn=connect (g_sockfd, (struct sockaddr *) addr, sizeof (struct sockaddr));
    unsigned wait = 1, deadline=10 ;
    while (conn == -1)
    {
        if(wait==deadline)
        {
            conn=connect (g_sockfd, (struct sockaddr *) addr, sizeof (struct sockaddr));
            wait=0;
           // deadline*=2;
            //if (deadline >  UINT_MAX)
             //   deadline=0;
            if(conn==-1)
            {
                perror ("[init] connect");  
                printf ("[init] check if mldonkey is currently running\n");
                printf("[init] connection failed, retrying in %d seconds\n", deadline);
            }
        }   
        wait++;
        // alternate green & red led
        if (i % 2)
            copyXPMArea (70, 50, 6, 6, 28, 50);
        else
            copyXPMArea (77, 50, 6, 6, 28, 50);
        i++;
        RedrawWindow ();
        sleep (1);
    }

    // if (g_verbose_mode)
    printf ("[init] connection established with %s:%d\n", C.hostname, PORT);
    // eat the 1st CoreProtocol message
    i = get_mldonkey_message ();
    if (i != 0)                 // to avoid undetermined behavior
        // (protocol's changes, ...)
    {
        printf ("[init] wrong message, please mail the author, seems like there was \
                a mlgui protocol change\nDying...\n");

        exit (1);
    }

    // send the GuiProtocolVersion (00) message to the core 
    char msg[] = {
        0x06, 0x00, 0x00, 0x00, // size (4 bytes)
        0x00, 0x00,             // opcode (2 bytes) : 00
        0x0F, 0x00, 0x00, 0x00  // protocol version = 15 (new in 0.3.4, yes, i do believe that someone will read this code)
    };

    if (send (g_sockfd, msg, 10, 0) == -1)
    {
        perror ("[init] send GuiProtocolVersion");
        exit (1);
    }

    // password message

    short int opc = 52;         // password message opcode 
    int lgt = (3 * sizeof (short int)) + strlen (C.password) + strlen (C.login);

    // printf ("lgt : %d\n", lgt);
    printf ("[init] login : %s\n", C.login);
    printf ("[init] pass : %s\n", C.password);

    send (g_sockfd, &lgt, sizeof (int), 0);
    send (g_sockfd, &opc, sizeof (short int), 0);

    opc = strlen (C.password);
    send (g_sockfd, &opc, sizeof (short int), 0);
    send (g_sockfd, C.password, opc, 0);

    opc = strlen (C.login);
    send (g_sockfd, &opc, sizeof (short int), 0);
    send (g_sockfd, C.login, opc, 0);

    // green led
    copyXPMArea (70, 50, 6, 6, 28, 50);
    RedrawWindow ();
    usleep (500000);

    // empty file page 
    copyXPMArea (100, 0, 56, 56, 4, 4);
    RedrawWindow ();

    // main area 
    AddMouseRegion (0, 44, 14, 58, 58);
    AddMouseRegion (1, 5, 5, 58, 15);
    // filenames 
    AddMouseRegion (2, 5, 15, 42, 25);
    AddMouseRegion (3, 5, 26, 42, 36);
    AddMouseRegion (4, 5, 37, 42, 47);
    AddMouseRegion (5, 5, 48, 42, 58);



    // create pidfile (/tmp/wmldonkey.pid);
    g_pidfile = malloc (22);      
    sprintf (g_pidfile, "/tmp/wmldonkey.%d", getpid ());
    printf ("[main] using pidfile: %s\n", g_pidfile);
    int pidfd = creat (g_pidfile, 0600);

    if (pidfd == -1)
    {
        perror ("create pidfile");
        exit (1);
    }
    close (pidfd);


    // shmem init 
    key_t cle;

    if ((cle = ftok (g_pidfile, SHMKEY1)) == -1)
    {
        perror ("[main] get key1");
        exit (1);
    }


    g_F.shmid1 = shmget (cle, sizeof (short int) * 3, IPC_EXCL | IPC_CREAT | 0660);
    if (g_F.shmid1 == -1)
    {
        perror ("[main] get g_F.shmid1");
        quit_proc (0);
    }
    // nb[0] = nb files , nb[1] = max files before resize, nb[2]=lock flag (sorry :o)
    if ((g_F.nb = shmat (g_F.shmid1, NULL, 0)) == (void *) -1)
    {
        perror ("[main] attach g_F.nb");
        quit_proc (0);
    }
    g_F.nb[0] = 0;
    g_F.nb[1] = 1;

    if ((cle = ftok (g_pidfile, SHMKEY2)) == -1)
    {
        perror ("[main] get key2");
        quit_proc (0);
    }

    g_F.shmid2 = shmget (cle, sizeof (struct FileInfo) * g_F.nb[1], IPC_EXCL | IPC_CREAT | 0660);
    if (g_F.shmid2 == -1)
    {
        perror ("[main] get g_F.shmid2");
        quit_proc (0);
    }
    if ((g_F.files1 = shmat (g_F.shmid2, NULL, 0)) == (void *) -1)
    {
        perror ("[main] attach g_F.files1");
        quit_proc (0);
    }
    for (i = 0; i < g_F.nb[1]; i++)
    {
        g_F.files1[i].file_size = 0;
        g_F.files1[i].file_downloaded = 0;
        g_F.files1[i].down_rate[0] = '\0';
        g_F.files1[i].pref_name[0] = '\0';
    }
}


//----
short int get_mldonkey_message ()
{
    int lgt, i, j, tmp32, fileid;
    short int opcode, tmp16;
    long int tmp64;
    struct FileInfo f;


//    printf("[get msg] waiting\n");
    if (recv (g_sockfd, &lgt, 4, 0) < 4)
    {

        perror ("[get msg] read message on socket");
        kill (getppid (), SIGINT);
        end_child (0);
        // quit_proc (0);
        // close(g_sockfd);
    }
    recv (g_sockfd, &opcode, 2, 0);

//    printf ("[opcode : %d (lgt=%d)] : \n", opcode, lgt);

    switch (opcode)
    {
            // coreprotocol
        case 0:
            // printf("[get msg] got CoreProtocol [%d]\n", lgt);
            recv (g_sockfd, &tmp32, 4, 0);    // version
            // printf("[get msg] version : %d\n", tmp32);
            recv (g_sockfd, &tmp32, 4, 0);    // max to core 
            // printf("[get msg] max to core : %d\n", tmp32);
            recv (g_sockfd, &tmp32, 4, 0);    // max from core
            // printf("[get msg] max from core : %d\n", tmp32);
            break;

            // badpassword
        case 47:
            if (g_verbose_mode)
                printf ("[get msg] BADPASSWORD!\n");
            printf ("Bad password or login name\n");
            printf ("Please run wmldonkey --setup\n");
            opcode = -2;
            break;
            // clienstats
        case 25:
        case 37:
        case 39:
        case 49:
            // printf ("[get msg] got ClientStats [%d]\n", lgt);
            recv (g_sockfd, &tmp64, 8, 0);    // upload cnt
            recv (g_sockfd, &tmp64, 8, 0);    // down cnt
            recv (g_sockfd, &tmp64, 8, 0);    // shared cnt
            recv (g_sockfd, &tmp32, 4, 0);    // shared fis
            recv (g_sockfd, &tmp32, 4, 0);    // tcp up
            // printf("tcp up : %d\n", tmp32);
            recv (g_sockfd, &tmp32, 4, 0);    // tcp down
            // printf("tcp down : %d\n", tmp32);
            recv (g_sockfd, &tmp32, 4, 0);    // udp up
            // printf("udp up : %d\n", tmp32);
            recv (g_sockfd, &tmp32, 4, 0);    // udp down 
            // printf("udp down : %d\n", tmp32);
            recv (g_sockfd, &tmp32, 4, 0);    // nb downloading files
            recv (g_sockfd, &tmp32, 4, 0);    // nb downloaded files

            recv (g_sockfd, &tmp16, 2, 0);    // protocol list's 
            int *list = malloc (tmp16 * sizeof (int));

            for (i = 0; i < (int) tmp16; i++)
            {
                recv (g_sockfd, &list[i], 4, 0);
            }
            free (list);
            break;

            // ConsoleMessage
        case 19:
            // printf("[get msg] ConsoleMessage [%d]\n", lgt);
            recv (g_sockfd, &tmp16, 2, 0);
            g_trash = realloc (g_trash, tmp16 + 1);
            recv (g_sockfd, g_trash, tmp16, 0);
            g_trash[tmp16] = '\0';
            break;

            // networkinfos
        case 20:
            // printf("[get msg] NetworkInfos [%d]\n", lgt);
            recv (g_sockfd, &tmp32, 4, 0);    // network id
            recv (g_sockfd, &tmp16, 2, 0);    // network name
            g_trash = realloc (g_trash, tmp16 + 1);
            recv (g_sockfd, g_trash, tmp16, 0);
            g_trash[tmp16] = '\0';

            unsigned char c;    // network status

            recv (g_sockfd, &c, 1, 0);

            recv (g_sockfd, &tmp16, 2, 0);    // conf file
            g_trash = realloc (g_trash, tmp16 + 1);
            recv (g_sockfd, g_trash, tmp16, 0);
            g_trash[tmp16] = '\0';

            recv (g_sockfd, &tmp64, 8, 0);    // uploaded bytes
            recv (g_sockfd, &tmp64, 8, 0);    // downloaded bytes


            break;

            // file info
        case 7:
        case 40:
        case 43:
        case 52:
            printf ("[get msg] got file info\n");
            // lock struct, father will use his local copy to draw
            // (my_files)
            // printf ("[get msg] locking struct\n");
            g_F.nb[2] = 1;

            if (g_verbose_mode)
                printf ("[get msg] DOWNLOADING FILE : candidate for file[%d] ", g_F.nb[0]);



            recv (g_sockfd, &f.file_id, 4, 0);    // [file id]
            recv (g_sockfd, &tmp32, 4, 0);    // network id

            recv (g_sockfd, &tmp16, 2, 0);    // list of filenames 
            short int plop = tmp16;

            for (j = 0; j < plop; j++)
            {

                recv (g_sockfd, &tmp16, 2, 0);    // filename[j] length
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0); // filename[j]
                g_trash[tmp16] = '\0';
            }

            // MD4
            g_trash = realloc (g_trash, 16);
            recv (g_sockfd, g_trash, 16, 0);

            recv (g_sockfd, &f.file_size, 4, 0);  // [file size]
            recv (g_sockfd, &f.file_downloaded, 4, 0);    // [file
            // downloaded]
            recv (g_sockfd, &tmp32, 4, 0);    // nb src
            recv (g_sockfd, &tmp32, 4, 0);    // nb clients
            recv (g_sockfd, &f.file_state, 1, 0); // [file state]

            if (f.file_state == 6)  // if aborting
            {
                // reason of abortion
                recv (g_sockfd, &tmp16, 2, 0);
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';
            }

            recv (g_sockfd, &tmp16, 2, 0);    // chunks
            g_trash = realloc (g_trash, tmp16 + 1);
            recv (g_sockfd, g_trash, tmp16, 0);
            g_trash[tmp16] = '\0';

            recv (g_sockfd, &tmp16, 2, 0);    // availability
            g_trash = realloc (g_trash, tmp16 + 1);
            recv (g_sockfd, g_trash, tmp16, 0);
            g_trash[tmp16] = '\0';


            recv (g_sockfd, &tmp16, 2, 0);    // down rate
            if (tmp16 > 11)
            {
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                strncpy (f.down_rate, g_trash, 11);
                f.down_rate[11] = '\0';
            }
            else
            {
                recv (g_sockfd, f.down_rate, tmp16, 0);
                f.down_rate[tmp16] = '\0';
            }


            recv (g_sockfd, &tmp16, 2, 0);    // times

            plop = tmp16;
            for (j = 0; j < plop; j++)
            {
                recv (g_sockfd, &tmp16, 2, 0);
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';
            }

            recv (g_sockfd, &tmp16, 2, 0);    // file age
            g_trash = realloc (g_trash, tmp16 + 1);
            recv (g_sockfd, g_trash, tmp16, 0);
            g_trash[tmp16] = '\0';

            recv (g_sockfd, &c, 1, 0);    // file format
            if (c == 0)         // - unknow -
            {;
            }
            else if (c == 1)    // - generic -
            {
                recv (g_sockfd, &tmp16, 2, 0);    // extension
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp16, 2, 0);    // kind
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';
            }
            else if (c == 2)    // - video -
            {
                recv (g_sockfd, &tmp16, 2, 0);    // video codec
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp32, 4, 0);    // width 
                recv (g_sockfd, &tmp32, 4, 0);    // height
                recv (g_sockfd, &tmp32, 4, 0);    // fps
                recv (g_sockfd, &tmp32, 4, 0);    // bitrate
            }
            else if (c == 3)    // - mp3 -
            {
                recv (g_sockfd, &tmp16, 2, 0);    // id3 title
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp16, 2, 0);    // id3 artist
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp16, 2, 0);    // id3 album
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp16, 2, 0);    // id3 year
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp16, 2, 0);    // id3 comment
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                g_trash[tmp16] = '\0';

                recv (g_sockfd, &tmp32, 4, 0);    // id3 track
                recv (g_sockfd, &tmp32, 4, 0);    // id3 genre
            }

            recv (g_sockfd, &tmp16, 2, 0);    // [pref filename]
            if (tmp16 > 255)
            {
                g_trash = realloc (g_trash, tmp16 + 1);
                recv (g_sockfd, g_trash, tmp16, 0);
                strncpy (f.pref_name, g_trash, 255);
                f.pref_name[255] = '\0';
            }
            else
            {
                recv (g_sockfd, f.pref_name, tmp16, 0);
                f.pref_name[tmp16] = '\0';
            }


            recv (g_sockfd, &tmp32, 4, 0);    // last seen
            if (opcode >= 52)   // it wasn't there before :o
                recv (g_sockfd, &tmp32, 4, 0);    // priority

            if (g_verbose_mode)
            {
                printf ("id : %d ", f.file_id);
                printf ("(state : %d) ", f.file_state);
                printf ("(@ %s o/s) ", f.down_rate);
                printf (" -> %s\n", f.pref_name);
            }

            // search if file exists, delete it if aborting or canceling


            // remove file from list
            if (f.file_state == 4 || f.file_state == 6 || f.file_state == 3)
            {
                if (g_verbose_mode)
                    printf ("[get msg] erasing file (if necessary) .... ");
                for (i = 0; i < g_F.nb[0]; i++)
                {
                    if (g_F.files1[i].file_id == f.file_id)
                        break;
                }
                if (i < g_F.nb[0])   // found it, replacing it by last entry
                {
                    if (g_verbose_mode)
                        printf ("[get msg] found it ! \n");
                    g_F.files1[i].file_id = g_F.files1[g_F.nb[0] - 1].file_id;
                    g_F.files1[i].file_state = g_F.files1[g_F.nb[0] - 1].file_state;
                    g_F.files1[i].file_size = g_F.files1[g_F.nb[0] - 1].file_size;
                    g_F.files1[i].file_downloaded = g_F.files1[g_F.nb[0] - 1].file_downloaded;
                    sprintf (g_F.files1[i].down_rate, "%s", g_F.files1[g_F.nb[0] - 1].down_rate);
                    sprintf (g_F.files1[i].pref_name, "%s", g_F.files1[g_F.nb[0] - 1].pref_name);

                    g_F.nb[0]--; // one down :o
                }
                else if (g_verbose_mode)
                    printf ("[get msg] got a shared files, don't care\n");
            }
            else                
            {
		// adding new file at end of array, we don't
		// want downloaded & commited files
                if (g_verbose_mode)
                    printf ("[get msg] searching file, array : 0->%d\n", g_F.nb[0]);
                // search
                for (i = 0; i < g_F.nb[0] && g_F.files1[i].file_id != f.file_id; i++)
                {
                    ;
                    // printf ("file[%d] :(%d) %s\n", i,
                    // g_F.files1[i].file_id,
                    // g_F.files1[i].pref_name);
                }

                // update if present
                if (g_F.files1[i].file_id == f.file_id)
                {
                    if (g_verbose_mode)
                        printf ("[get msg] found it (%s) ! updating\n", g_F.files1[i].pref_name);
                    // g_F.files1[i].file_id=f.file_id;
                    g_F.files1[i].file_state = f.file_state;
                    g_F.files1[i].file_size = f.file_size;
                    g_F.files1[i].file_downloaded = f.file_downloaded;
                    sprintf (g_F.files1[i].down_rate, "%s", f.down_rate);
                    // sprintf(g_F.files1[g_F.nb[0]].pref_name, "%s",
                    // f.pref_name);
                }
                else            // else add it
                {
                    if (g_verbose_mode)
                        printf ("[get msg] adding file\n\n\n");
                    g_F.files1[g_F.nb[0]].file_id = f.file_id;
                    g_F.files1[g_F.nb[0]].file_state = f.file_state;
                    g_F.files1[g_F.nb[0]].file_size = f.file_size;
                    g_F.files1[g_F.nb[0]].file_downloaded = f.file_downloaded;
                    sprintf (g_F.files1[g_F.nb[0]].down_rate, "%s", f.down_rate);
                    sprintf (g_F.files1[g_F.nb[0]].pref_name, "%s", f.pref_name);
                    g_F.nb[0]++;

                    // we've just use our last free slot
                    if (g_F.nb[0] == g_F.nb[1])
                    {
                        if (g_verbose_mode)
                        {
                            printf ("[get msg] all slots are full, reallocating\n");
                            printf ("[get msg] %d sends SIGUSR1 to %d\n", getpid (), getppid ());
                        }

                        if (kill (getppid (), SIGUSR1) == -1)
                        {
                            perror ("[get msg] send sigusr1");
                        }
                        // shmdt (g_F.nb);
                        // shmdt (g_F.files1);
                        // exit (1);

                        // unlock struct for father (we don't really care,
                        // because the child is gonna die :o )
                        // printf ("[get msg] unlocking struct\n");
                        g_F.nb[2] = 0;

                        opcode = -3;    // 
                    }
                }
            }
            // unlock struct 
            // printf ("[get msg] unlocking struct\n");
            g_F.nb[2] = 0;

            break;

            // download update
        case 46:
            // lock struct
            g_F.nb[2] = 1;
            if (g_verbose_mode)
                printf ("[get msg] DOWNLOAD UPDATE for file : ");
            recv (g_sockfd, &fileid, 4, 0);
            if (g_verbose_mode)
                printf ("%d, ", fileid);

            for (i = 0; i < g_F.nb[0]; i++)
            {
                if (g_F.files1[i].file_id == fileid)
                    break;
            }
            if (g_F.files1[i].file_id == fileid)
            {
                recv (g_sockfd, &g_F.files1[i].file_downloaded, 4, 0);
                recv (g_sockfd, &tmp16, 2, 0);
                recv (g_sockfd, g_F.files1[i].down_rate, tmp16, 0);
                g_F.files1[i].down_rate[tmp16] = '\0';
                // last seen, don't care
                recv (g_sockfd, &tmp32, 4, 0);
                if (g_verbose_mode)
                    printf ("down rate for %s: %s\n", g_F.files1[i].pref_name, g_F.files1[i].down_rate);
                if (strcmp (g_F.files1[i].down_rate, "0.0") == 0)
                {
                    if (g_F.files1[i].file_downloaded == g_F.files1[i].file_size)
                        g_F.files1[i].file_state = 2;
                }
            }
            else                // this should not happen 
            {
                // we've already red opcode (2) & file id (4)
                g_trash = realloc (g_trash, lgt - 6);
                recv (g_sockfd, g_trash, lgt - 6, 0);
            }
            // unlock struct
            g_F.nb[2] = 0;
            break;

        default:
            // printf("[get msg] ploped opcode %d [%d bytes]\n", opcode, lgt-2);
            g_trash = realloc (g_trash, lgt - 2);
            i = recv (g_sockfd, g_trash, lgt - 2, 0);
            break;
    };

    return opcode;
}

// ---
void draw_page (struct Files *my_files, struct Page *P, struct Config C)
{
    static int old = 0;
    int nb_files, nb_pages, i, j, y;
    float x;
    char percent[3];

    nb_files = g_F.nb[0];

    // when a file arrives or leaves the files list, redraw 
    // a blank page to avoid display glitches
    if (old != nb_files)
    {
        copyXPMArea (100, 0, 56, 56, 4, 4);
        RedrawWindow ();
    }
    old = nb_files;

    nb_pages = nb_files / 4;
    if (nb_files % 4)
        nb_pages++;

    // if F is unlocked, copy g_F to my_files, else we'll use last "image"
    if (!g_F.nb[2])
    {
        filesCopy (my_files, &g_F);
    }

    y = 15;    // 1st line offset
    for (i = P->page * 4, j = 0; i < my_files->nb[0] && j < 4; i++, j++)
    {
        /* let's draw */
        
        // filename
        draw_file_line (my_files->files1[i], 42, P->pos[j], y);

        // percent done
        if (my_files->files1[i].file_state == 2)
            draw_file_percent (my_files->files1[i], "ok", 45, y);
        else
        {
            // convert progression into percents
            x = ((float) my_files->files1[i].file_downloaded / my_files->files1[i].file_size) * 100;

            sprintf (percent, "%02d", (int) x);
            percent[2] = '\0';
            if (x == 100)
                draw_file_percent (my_files->files1[i], "ok", 45, y);

            draw_file_percent (my_files->files1[i], percent, 45, y);
        }

        // update position variables 
        if (P->pos[j] == (-strlen (my_files->files1[i].pref_name) * 6)) // at end
            P->pos[j] = 43;
        else if (P->scroll[j])
            P->pos[j]--;
        P->fileid[j] = my_files->files1[i].file_id;

        y += 11;   // goto next line
    }


    // blank lines 
    struct FileInfo empty;

    empty.file_state = 0;
    strcpy (empty.down_rate, "0.0");
    strcpy (empty.pref_name, "      ");

    while (j < 4)
    {
        P->fileid[j] = -1;
        P->pos[j] = 4;
        P->scroll[j] = C.line_scroll;
        draw_file_line (empty, 42, 4, y);
        draw_file_percent (empty, "  ", 45, y);
        j++;
        y += 11;
    }
    
    /* it worked ! */
}




// ---

void draw_file_line (struct FileInfo F, int limit, int x, int y)
{
    int charset1, charset2, i;

    // choose text color 
    switch (F.file_state)
    {
        case 0:
            charset1 = 64;
            charset2 = 0;
            break;
        case 1:
            charset1 = 99;
            charset2 = 10;
            break;
        case 2:
            charset1 = 169;
            charset2 = 30;
            break;
        default:
            charset1 = 64;
            charset2 = 0;
            break;
    };

    if (strcmp ("0.0", F.down_rate) != 0)
    {
        charset1 = 134;
        charset2 = 20;
    }

    char d;
    int last = 0, w = 0;

    for (i = 0; i < strlen (F.pref_name) && !last; i++)
    {

        // last character is kinda tricky
        if (x + (i + 1) * 6 >= limit)
        {
            // how many pixels do we have left ? 
            w = 7 - (limit - (x + i * 6));

            last = 1;
            w++;

            /* 
               sometimes 'w' happens to be > 7, and X does not like it very much :/ i'll fix this someday. */
            if (7 - w < 0)
            {
                // printf ("[dsp] BOOOOM\n");
                w = 7;
            }
        }

        // copyXPMArea(X_SRC, Y_SRC, WIDTH, HEIGHT, X_DST, Y_DST);
        d = F.pref_name[i];

        if (d >= 'A' && d <= 'Z')
        {
            d -= 'A';
            copyXPMArea (d * 6, charset1, 7 - w, 9, x + i * 6, y);
        }
        else if (d >= 'a' && d <= 'z')
        {
            d -= 'a';
            copyXPMArea (d * 6, charset1 + 10, 7 - w, 9, x + i * 6, y);
        }
        else if (d >= ' ' && d <= '9')
        {
            d -= ' ';
            copyXPMArea (d * 6, charset1 + 20, 7 - w, 9, x + i * 6, y);
        }
        else if (d >= '[' && d <= '`')
        {
            d -= '[';
            copyXPMArea (64 + d * 6, charset2, 7 - w, 9, x + i * 6, y);
        }
        else
            copyXPMArea (0, 84, 7 - w, 9, x + i * 6, y);
    }
}

// starts drawing @ (x,y)
void draw_file_percent (struct FileInfo F, char *str, int x, int y)
{
    int charset = 64, i;
    char d;

    switch (F.file_state)
    {
        case 0:
            charset = 64;
            break;
        case 1:
            charset = 99;
            break;
        case 2:
            charset = 169;
            break;
        default:
            charset = 64;
            break;
    };

    if (strcmp ("0.0", F.down_rate) != 0)
        charset = 134;

    for (i = 0; i < strlen (str); i++)
    {
        d = str[i];

        /* "ok" == 100% */
        if (d >= 'a' && d <= 'z')
        {
            d -= 'a';
            copyXPMArea (d * 6, charset + 10, 7, 9, x + i * 6, y);
        }
        /* numbers 00 -> 99 % */
        else if (d >= ' ' && d <= '9')
        {
            d -= ' ';
            copyXPMArea (d * 6, charset + 20, 7, 9, x + i * 6, y);
        }
        /* for blank line */
        else
            copyXPMArea (0, 84, 7, 9, x + i * 6, y);
    }
}





void launch_child (int fd)
{
    if (g_verbose_mode)
        printf ("[father] go son!\n");
    g_child_pid = fork ();

    if (!g_child_pid)
    {
        signal (SIGUSR1, dummy);    // overwrite existing handler
        signal (SIGCHLD, dummy);
        signal (SIGSEGV, dummy);
        signal (SIGPIPE, disconnect);
        signal (SIGTERM, end_child);
        signal (SIGINT, int_child);

        if (g_verbose_mode)
            printf ("[child] ready to go (pid : %d)\n", getpid ());
        int i = 42;

        while (i >= 0)
        {
            i = get_mldonkey_message (fd);
            if (i == -1)        // socket issues
            {
                if (g_verbose_mode)
                    perror ("[child] get mldonkey msg");
                shmdt (g_F.nb);
                shmdt (g_F.files1);
                if (g_verbose_mode)
                    printf ("[child] %d dying...\n", getpid ());

                kill (getppid (), SIGPIPE);

            }
            else if (i == -2)   // BADPASSWORD
            {
                shmdt (g_F.nb);
                shmdt (g_F.files1);
                if (g_verbose_mode)
                    printf ("[child] %d dying...\n", getpid ());

                kill (getppid (), SIGTERM);
            }
            else if (i == -3)   // REALLOC
            {
                shmdt (g_F.nb);
                shmdt (g_F.files1);
                if (g_verbose_mode)
                    printf ("[child] %d dying...\n", getpid ());

                // kill(getppid(), SIGTERM);
            }
        }
        exit (0);
    }
    else
    {
        if (g_verbose_mode)
            printf ("[father] child got : %d\n", g_child_pid);
    }
}

void quit_proc (int val)
{
    if (val == SIGPIPE)
        printf ("[quit proc] core disconnected\n");
    if (g_verbose_mode)
        printf ("[quit proc]: cleaning...\n");
    printf ("[quit] got signal : %d\n", val);

    close (g_sockfd);
    free (g_trash);

    if (g_child_pid > 0)        // trying to kill init is bad (and it could 
        // happen in this stupid program, believe me) :o.
    {
        kill (g_child_pid, SIGTERM);
        wait (NULL);
    }


    unlink (g_pidfile);

    /* bye shm */
    shmdt (g_F.nb);
    shmdt (g_F.files1);
    if (shmctl (g_F.shmid1, IPC_RMID, 0) == -1)
        perror ("[quit proc] delete shm1");
    if (shmctl (g_F.shmid2, IPC_RMID, 0) == -1)
        perror ("[quit proc] delete shm2 ");


    printf ("[quit proc] cleaned up all structures, Bye...\n");
    exit (0);
}

void child_died (int val)
{
    signal (SIGCHLD, child_died);
    g_child_is_dead = 1;
    if (g_verbose_mode)
        perror ("child is dead");

    wait (NULL);                // fuck zombies :o
}

void disconnect (int sig)
{
    printf ("core disconnected, leaving\n");
    kill (getppid (), SIGPIPE);
}

// for sigterm on child2
void end_child (int sig)
{
    if (g_verbose_mode)
        printf ("[child] ending\n");
    shmdt (g_F.nb);
    shmdt (g_F.files1);
    exit (1);
}

void int_child (int sig)
{
    printf ("[child] interrupted\n");
    kill (getppid (), SIGINT);
    exit (1);
}

void dummy (int sig)
{
    if (g_verbose_mode)
    {
        perror ("child received sig");
        printf ("signum : %d\n", sig);
    }
}

void realloc_files1 (int val)
{
    
    // reinstall handler
    signal (SIGUSR1, realloc_files1);

    if (g_verbose_mode)
        printf ("[realloc] reallocing (old size : %d), child pid : %d\n", g_F.nb[1], g_child_pid);


    // we need to create a new shm segment
    // save old segment
    struct FileInfo *tmp = malloc (sizeof (struct FileInfo) * g_F.nb[1]);
    tmp = memcpy (tmp, g_F.files1, sizeof (struct FileInfo) * g_F.nb[1]);

    // destroy old segment
    shmdt (g_F.files1);
    shmctl (g_F.shmid2, IPC_RMID, 0);

    // respawn a larger one (size + 10)
    if (g_verbose_mode)
        printf ("[resize shm] generating new key with %s:%d\n", g_pidfile, SHMKEY2);
    key_t cle = ftok (g_pidfile, SHMKEY2);

    if (cle == -1)
    {
        perror ("[resize shm] create key");
        quit_proc (0);
        exit (1);
    }
    g_F.shmid2 = shmget (cle, sizeof (struct FileInfo) * (g_F.nb[0] + 10), IPC_EXCL | IPC_CREAT | 0660);
    if (g_F.shmid2 == -1)
    {
        perror ("[resize shm] getting F.files1");
        quit_proc (0);
    }
    g_F.files1 = shmat (g_F.shmid2, 0, 0);
    if (g_F.files1 == (void *) -1)
    {
        perror ("[resize shm] attaching F.files1");
        quit_proc (0);
    }
    g_F.files1 = memcpy (g_F.files1, tmp, sizeof (struct FileInfo) * g_F.nb[1]);

    g_F.nb[1] = g_F.nb[0] + 10;
    free (tmp);

    if (g_verbose_mode)
        printf ("[resize shm] new size : %d, fd : %d)\n", g_F.nb[1], g_sockfd);
}


void reconnect(int val)
{
    
    printf ("[quit proc] *****************core disconnected\n");
    if (g_verbose_mode)
        printf ("[quit proc]: cleaning...\n");
    
    close (g_sockfd);
    free (g_trash);

    if (g_child_pid > 0)        // trying to kill init is bad (and it could 
        // happen in this stupid program, believe me) :o.
    {
        kill (g_child_pid, SIGTERM);
        wait (NULL);
    }


    unlink (g_pidfile);

    /* bye shm */
    shmdt (g_F.nb);
    shmdt (g_F.files1);
    if (shmctl (g_F.shmid1, IPC_RMID, 0) == -1)
        perror ("[quit proc] delete shm1");
    if (shmctl (g_F.shmid2, IPC_RMID, 0) == -1)
        perror ("[quit proc] delete shm2 ");


    printf ("[quit proc] cleaned up all structures, Bye...\n");
    exit (0);
}

static void print_usage (void)
{
    printf ("\n\nwmldonkey dockapp version: %s\n\n", VERSION);
    printf ("\nusage : \n\n");
    printf ("\t--setup                                   configure login/password (values are stored in config file)\n");
    printf ("\t--ident                                   interactive login (see manual for details)\n");
    printf ("\t--no-passwd                               interactive login with blank password\n\n");
    printf ("\t--display <display name>                  X server to connect to\n");
    printf ("\t--host <hostname>                         connect to <hostname>\n");
    printf ("\t--speed <n>                               scrolling speed (1->10) (lower is faster)\n");
    printf ("\t--line-scroll <false/off/0, true/on/1>    \n");
    printf ("\t--page-scroll <period 0/1->300>           time in seconds between automatic page change (0=don't change)\n");
    printf ("\t--verbose -v                              verbose mode\n\n");
//    printf ("\t--file -f <filename>                      config file to use\n");
    printf ("\t--help -h                                 this help\n");
}

void ParseCMDLine (int argc, char *argv[], struct Config *C, struct Page *P)
{

    int i, j;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp (argv[i], "--display"))
        {
            ++i;                /* -display is used in wmgeneral */
        }
        else if (!strcmp (argv[i], "--host"))
        {
            C->hostname = malloc (strlen (argv[i + 1]) + 1);
            strcpy (C->hostname, argv[i + 1]);
            // (*hostname)[strlen (argv[i + 1])] = '\0';
            i++;
        }
        else if (!strcmp (argv[i], "--speed"))
        {
            C->speed = strtol (argv[i + 1], NULL, 10);
            if (C->speed < 1 || C->speed > 10)
            {
                printf ("[parse cmd line] speed=%d out of range, falling back on '5'\n", C->speed);
                C->speed = 5;
            }
            i++;
        }
        else if (!strcmp (argv[i], "-V") || !strcmp (argv[i], "--version"))
        {
            printf ("wmldonkey v %s\n", VERSION);
            exit (0);
        }
        else if (!strcmp (argv[i], "-v") || !strcmp (argv[i], "--verbose"))
        {
            g_verbose_mode = 1;
        }
        else if (!strcmp (argv[i], "--line-scroll"))
        {
            if (!strcmp (argv[i + 1], "false") || !strcmp (argv[i + 1], "off") || !strcmp (argv[i + 1], "0"))
                C->line_scroll = 0;

            else if (!strcmp (argv[i + 1], "true") || !strcmp (argv[i + 1], "on") || !strcmp (argv[i + 1], "1"))
                C->line_scroll = 1;

            else
            {
                print_usage ();
                exit (1);
            }
            for (j = 0; j < 4; j++)
                P->scroll[j] = C->line_scroll;
            i++;
        }
        else if (!strcmp (argv[i], "--page-scroll"))
        {
            printf ("[parse cmd line] page scroll\n");
            char *end = NULL;

            j = strtol (argv[i + 1], &end, 10);

            P->page_scroll = j;

            if (strcmp (end, "") || j > 300)
            {
                // free (end);
                print_usage ();
                exit (1);
            }
            i++;
        }
        // support for alternate config file is out - if you want it back, contact me :o
        /* else if (!strcmp (argv[i], "-f") || !strcmp (argv[i], "--file")) { printf ("[parse cmd line] choosing conf file\n");
           if (argv[i + 1][0] == '/') // absolute path name { C->rcfilename = malloc (strlen (argv[i + 1]) + 1);
           sprintf(C->rcfilename, "%s", argv[i + 1]); C->rcfilename[strlen (argv[i + 1])] = '\0'; } else { if (argv[i + 1][0]
           == '~') { argv[i + 1][0] = '/'; // FIXME, this sucks }

           char *home = getenv ("HOME");

           C->rcfilename = malloc (strlen (home) + strlen (argv[i + 1]) + 1); sprintf (C->rcfilename, "%s/%s", home, argv[i +
           1]); printf ("[parse cmd line] user specified cfgfile: %s\n", C->rcfilename); }

           struct stat buf_stat;

           if (!stat (C->rcfilename, &buf_stat)) // if file exists readconf (C, P);

           i++; } */
        else if (!strcmp (argv[i], "--setup"))
        {
            printf ("[parse cmd line] let's configure\n");
            make_password (*C);
            exit (0);
        }
        else    // stupid user :o
        {
            print_usage ();
            exit (0);
        }
    }



}


void createDefaultCfg (struct Config *C)
{

    printf ("[create default cfg] welcome dear user :o\n");
    int fd;
    char *buf;

    fd = open (C->rcfilename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

    if (fd == -1)
    {
        perror ("[creat default cfg] open");
        quit_proc (0);
    }

    buf = malloc (300);
    sprintf (buf, "# .wmldonkeyrc - v0.3.4 ");       write (fd, buf, strlen (buf));
    sprintf (buf, "auto-generated config file\n\n"); write (fd, buf, strlen (buf));
    
    sprintf (buf, "#where is the mlcore running ?\n"); write (fd, buf, strlen (buf));
    sprintf (buf, "hostname=localhost\n");             write (fd, buf, strlen (buf));
    
    sprintf (buf, "line scroll=true\n");  write (fd, buf, strlen (buf));
    
    sprintf (buf, "#scrolling speed (1->10, lower is faster)\n");   write (fd, buf, strlen (buf));
    sprintf (buf, "speed=5\n");                                     write (fd, buf, strlen (buf));

    sprintf (buf, "#number of seconds between automatic page change\n");           write (fd, buf, strlen (buf));
    sprintf (buf, "#(one page shows 4 downloads, 0 means no automatic change)\n"); write (fd, buf, strlen (buf));
    sprintf (buf, "page scroll=0\n");                                              write(fd, buf, strlen(buf));  
    
    sprintf(buf, "#login & password (mlcore authentification)\n");             write(fd, buf, strlen(buf));  
    sprintf(buf, "#change these options with wmldonkey --setup or\n");         write(fd, buf, strlen(buf));       
    sprintf(buf, "#use de 'do it yourself' flavour : emacs ~/.wmldonkeyrc\n"); write(fd, buf, strlen(buf));       
    sprintf(buf, "#if you didn't specify login:password to mldonkey, \n");     write(fd, buf, strlen(buf));  
    sprintf(buf, "#juste leave'em to admin:<nothing, not even a space>\n");    write(fd, buf, strlen(buf));       
    sprintf(buf, "login=admin\npassword=");                                    write(fd, buf, strlen(buf));       
    
    close (fd);
    free (buf);
    chown (C->rcfilename, getuid (), -1);
}

void readconf (struct Config *C, struct Page *P)
{
    if (g_verbose_mode)
        printf ("[cfg] reading conf : %s\n", C->rcfilename);

    FILE *f;
    char *line = malloc (1);

    line[0] = '\0';
    int size1 = strlen (line) + 1, i, j;
    struct stat buf_stat;

    if (stat (C->rcfilename, &buf_stat) == -1)
    {
        printf ("[cfg] no configuration file found, creating default\n");
        createDefaultCfg (C);
    }


    /* TODO : s/fopen/open/ & friends */
    if ((f = fopen (C->rcfilename, "r")) != NULL)
    {
        while ((i = getline (&line, &size1, f)) != -1)
        {
            line[i - 1] = '\0';
            if (line[0] == '#');
            else
            {
                // fuck strtok() ! :o 
                for (i = 0; i < strlen (line) && line[i] != '='; ++i)
                    ;

                if (line[i] == '=')
                {
                    char *end;

                    if (!strncmp ("hostname", line, i - 1))
                        strcpy (C->hostname, &line[i + 1]);
                    else if (!strncmp ("page scroll", line, i - 1))
                    {
                        P->page_scroll = strtol (&line[i + 1], &end, 10);
                        if (strcmp (end, "") || P->page_scroll > 300)
                        {
                            // free (end);
                            printf ("[cfg] bad \"page scroll\" value, falling back on default value\n");
                            P->page_scroll = 0;
                        }
                    }
                    else if (!strncmp ("line scroll", line, i - 1))
                    {
                        if (!strcmp (&line[i + 1], "false") || !strcmp (&line[i + 1], "off") || !strcmp (&line[i + 1], "0"))
                            C->line_scroll = 0;
                        else if (!strcmp (&line[i + 1], "true") || !strcmp (&line[i + 1], "on") || !strcmp (&line[i + 1], "1"))
                            C->line_scroll = 1;
                        else
                        {
                            printf ("[cfg] bad \"line scroll\" value, falling back on default value\n");
                            C->line_scroll = 0;
                        }
                        for (j = 0; j < 4; j++)
                            P->scroll[j] = C->line_scroll;
                    }
                    else if (!strncmp ("speed", line, i - 1))
                    {
                        C->speed = strtol (&line[i + 1], NULL, 10);
                        if (C->speed < 1 || C->speed > 10)
                        {
                            printf ("[readconf] speed=%d out of range, falling back on '5'\n", C->speed);
                            C->speed = 5;
                        }

                    }
                    else if (!strncmp ("login", line, i - 1))
                    {
                        printf ("[readconf] login=%s\n", &line[i + 1]);
                        C->login = malloc (strlen (&line[i + 1]));
                        strcpy (C->login, &line[i + 1]);
                    }
                    else if (!strncmp ("password", line, i - 1))
                    {
                        printf ("[readconf] password="/*, &line[i + 1]*/);
                        // what an useful loop !
                        int j;
                        for(j=0 ; j<strlen(&line[i + 1]) ; j++)
                            putchar('*'); 
                        puts("\n");
                        C->password = malloc (strlen (&line[i + 1]));
                        strcpy (C->password, &line[i + 1]);
                    }

                }
                else            // unknow, don't care
                if (g_verbose_mode)
                    printf ("[cfg] unknow option : <%s>, skipping\n", line);
            }
        }
        fclose (f);
    }

}



void filesCopy (struct Files *dst, struct Files *src)
{
    // TODO : use memcopy(), cuz' i worth it :o
    dst->shmid1 = src->shmid1;  // we don't
    dst->shmid2 = src->shmid2;  // really care
    dst->nb[0] = src->nb[0];
    dst->nb[1] = src->nb[1];
    dst->nb[2] = src->nb[2];

    dst->files1 = realloc (dst->files1, sizeof (struct FileInfo) * (dst->nb[1]));
    memcpy (dst->files1, src->files1, sizeof (struct FileInfo) * (dst->nb[1]));

}



void make_password (struct Config C)
{
    // check for config files and stuff
    struct stat buf_stat;

    if (stat (C.rcfilename, &buf_stat) == -1)
    {
        printf ("[cfg] no configuration file found, creating default\n");
        createDefaultCfg (&C);
    }

    // get login & pass 
    struct termios old, new;
    int n = 255, i, nread, cpt;
    char *login, *pass, *buf;

    login = malloc (256);
    pass = malloc (256);

    printf ("login : ");
    fflush (stdout);
    i = read (STDIN_FILENO, login, 255);
    login[i - 1] = '\0';


    // let's grab & encrypt the password
    printf ("password (will not be echoed) : ");
    fflush (stdout);

    // Turn echoing off and fail if we can't. 
    if (tcgetattr (fileno (stdin), &old) != 0)
    {
        perror ("[setup]");
        exit (1);
    }
    new = old;
    new.c_lflag &= ~ECHO;
    if (tcsetattr (fileno (stdin), TCSAFLUSH, &new) != 0)
    {
        perror ("[setup]");
        exit (1);
    }

    // Read the password. 
    nread = getline (&pass, &n, stdin);
    pass[nread - 1] = '\0';     // chop()

    // Restore terminal. 
    (void) tcsetattr (fileno (stdin), TCSAFLUSH, &old);

    printf ("[setup] plop\n");
    /* you don't want to read this, please go away :o */
    // open config file
    stat (C.rcfilename, &buf_stat);

    printf ("[setup] file size : %ld\n", buf_stat.st_size + 1);

    buf = malloc (buf_stat.st_size + 1);
    int fd = open (C.rcfilename, O_RDWR);

    read (fd, buf, buf_stat.st_size);

    /* black magic happens */
    // count number of lines in file
    cpt = 0;
    for (i = 0; i < strlen (buf); i++)
        if (buf[i] == '\n')
            cpt++;

    printf ("[setup] nb lines : %d\n", cpt);

    int j, buf_pos = 0;
    char *p;
    char **rcfile = malloc ((cpt) * sizeof (char *));

    // copy file content to buffer
    for (i = 0; i < cpt; i++)
    {

        j = 0;
        for (p = &buf[buf_pos]; (*p) != '\n'; p++, j++)
            ;

        // printf("[setup] rcfile[%d], allocating : %d\n", i, j+2);
        rcfile[i] = malloc (j + 2); // +2 -> \n\0 
        strncpy (rcfile[i], &buf[buf_pos], j + 1);
        rcfile[i][j + 1] = '\0';
        j++;
        buf_pos += j;
    }

    // erase file
    printf ("[setup] erasing\n");
    if (ftruncate (fd, 0) == -1)
        perror ("[setup] truncate");
    // rewind ;-)
    if ((i = lseek (fd, 0, SEEK_SET)) == -1)
        perror ("[setup] lseek");

    int pass_ok = 0, login_ok = 0;

    // write new content to file
    for (i = 0; i < cpt; i++)
    {
        if (!strncmp (rcfile[i], "login=", strlen ("login=")) && !login_ok)
        {
            sprintf (buf, "login=%s\n", login);
            if (write (fd, buf, strlen (buf)) == -1)
                perror ("[setup] write");
            login_ok = 1;       // means that we've just overwrite the login line
        }
        else if (!strncmp (rcfile[i], "password=", strlen ("password=")) && !pass_ok)
        {
            sprintf (buf, "password=%s\n", pass);
            if (write (fd, buf, strlen (buf)) == -1)
                perror ("[setup] write");
            pass_ok = 1;
        }
        else                    // other strings
        {
            if (write (fd, rcfile[i], strlen (rcfile[i])) == -1)
                perror ("[setup] write");
        }
    }
    /* damn, it worked */

    // maybe pass & login were not in the file before
    if (!login_ok)
    {
        sprintf (buf, "login=%s\n", login);
        if (write (fd, buf, strlen (buf)) == -1)
            perror ("[setup] write login");
    }
    if (!pass_ok)
    {
        sprintf (buf, "password=%s\n", pass);
        if (write (fd, buf, strlen (buf)) == -1)
            perror ("[setup] write pass");
    }

    // that's it, we're done
    close (fd);

    // clean up the mess
    for (i = 0; i < cpt; i++)
        free (rcfile[i]);
    free (rcfile);
    free (login);
    free (pass);
    free (buf);
}
