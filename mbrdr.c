/*                               -*- Mode: C -*- 
 * Copyright (C) 2024, Mats Bergstrom
 * $Id$
 * 
 * File name       : mbrdr.c
 * Description     : Modbus reader
 * 
 * Author          : Mats Bergstrom
 * Created On      : Thu Feb 29 20:01:00 2024
 * 
 * Last Modified By: Mats Bergstrom
 * Last Modified On: Tue Mar  5 06:51:12 2024
 * Update Count    : 90
 * Status          : $State$
 * 
 * $Locker$
 * $Log$
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <mosquitto.h>

#include "modbus.h"
#include "cfgf.h"

/* MQTT ID, broker and port */
#define MQTT_ID		"mbrdr"
#define MQTT_BROKER	"127.0.0.1"
#define MQTT_PORT	1883


#define MODBUS_ID	(1)
#define MODBUS_ADDR	"127.0.0.1"
#define MODBUS_PORT	(502)



int opt_v = 0;				/* Verbose printing */
int opt_n = 0;				/* NoActive, do not send mqtt data */
int opt_m = 0;				/* NoModbus, do not connect to modbus */

/*****************************************************************************/
/* Config file handling */
/* mqtt   <broker> <port> <id> */
/* modbus <addr>   <port> <id> */
/* delays <connect-delay> <read-timeout> <write-delay> */
/* intervals <ACTIVE-interval> <IDLE-interval> <STANDBY-interval> */

char* mqtt_broker = 0;
int   mqtt_port;
char* mqtt_id = 0;

int
set_mqtt( int argc, const char** argv)
{
    if ( argc != 4 )
	return -1;
    
    if (mqtt_broker)
	free( mqtt_broker );
    mqtt_broker = strdup( argv[1] );
    if ( !mqtt_broker || !*mqtt_broker )
	return -1;

    mqtt_port = atoi(argv[2]);
    if ( (mqtt_port < 1) || (mqtt_port > 65535) )
	return -1;

    if (mqtt_id)
	free( mqtt_id );
    mqtt_id = strdup( argv[3] );
    if ( !mqtt_id || !*mqtt_id )
	return -1;

    if ( opt_v )
	printf("mqtt %s %d %s\n", mqtt_broker, mqtt_port, mqtt_id);
    
    return 0;
}

int   modbus_id   = 1;
char* modbus_addr = 0;
int   modbus_port = 502;

int
set_modbus( int argc, const char** argv )
{
    if ( argc != 4 )
	return -1;
    
    if (modbus_addr)
	free( modbus_addr );
    modbus_addr = strdup( argv[1] );
    if ( !modbus_addr || !*modbus_addr )
	return -1;

    modbus_port = atoi(argv[2]);
    if ( (modbus_port < 1) || (modbus_port > 65535) )
	return -1;

    modbus_id = atoi(argv[3]);
    if ( (modbus_port < 0) || (modbus_port > 65535) )
	return -1;

    if ( opt_v )
	printf("modbus %s %d %d\n", modbus_addr, modbus_port, modbus_id);
    
    return 0;
}

unsigned int	modbus_connect_delay	= 5;
unsigned int	modbus_read_timeout	= 5;
unsigned int	modbus_write_delay	= 2;

int
set_delays( int argc, const char** argv )
{
    if ( argc != 4 )
	return -1;

    modbus_connect_delay = atoi(argv[1]);
    if  (modbus_connect_delay < 1)
	return -1;

    modbus_read_timeout = atoi(argv[2]);
    if  (modbus_read_timeout < 1)
	return -1;

    modbus_write_delay = atoi(argv[3]);
    if  (modbus_write_delay < 1)
	return -1;

    
    if ( opt_v )
	printf("delays %u %u %u\n",
	       modbus_connect_delay, modbus_read_timeout, modbus_write_delay );
    
    return 0;
}

unsigned int	ACTIVE_interval		= 120;	/*  2 min */
unsigned int	IDLE_interval		= 900;	/* 15 min */
unsigned int	STANDBY_interval	= 120;	/*  2 min */


int
set_intervals( int argc, const char** argv )
{
    if ( argc != 4 )
	return -1;

    ACTIVE_interval = atoi(argv[1]);
    if  (ACTIVE_interval < 1)
	return -1;

    IDLE_interval = atoi(argv[2]);
    if  (IDLE_interval < 1)
	return -1;

    STANDBY_interval = atoi(argv[3]);
    if  (STANDBY_interval < 1)
	return -1;

    
    if ( opt_v )
	printf("intervals %u %u %u\n",
	       ACTIVE_interval, IDLE_interval, STANDBY_interval );
    
    return 0;
}

cfgf_tagtab_t tagtab[] = {
			  {"mqtt",	3, set_mqtt },
			  {"modbus",	3, set_modbus },
			  {"delays",	3, set_delays },
			  {"intervals",	3, set_intervals },
			  {0,0,0}
};


/*****************************************************************************/
/* Misc support */

void
my_gettime(struct timespec* ts)
	/* Get current time or bomb */
{
    int i;
    i = clock_gettime(CLOCK_REALTIME, ts);
    if ( i ) {
	perror("clock_gettime: ");
	exit( EXIT_FAILURE );
    }
}

void
add_time_sec(struct timespec* t, const struct timespec* now, unsigned dt)
{
    t->tv_sec  = now->tv_sec + dt;
    t->tv_nsec = now->tv_nsec;
}

void
my_sleep( const struct timespec* ts )
{
    int i;
    i = clock_nanosleep( CLOCK_REALTIME, TIMER_ABSTIME, ts, 0);
    if ( i ) {
	perror("clock_nanosleep: ");
	exit( EXIT_FAILURE );
    }
}

int
is_past_time( struct timespec* reset_ts, unsigned int dt )
{
    struct timespec now_ts;
    my_gettime( &now_ts );
    /* Have we passed the reset time yet? */
    if ( now_ts.tv_sec < reset_ts->tv_sec ) return 0; /* Nope */

    /* Yes, set the next reset time */
    add_time_sec( reset_ts, &now_ts, dt );
    return 1;
}

/*****************************************************************************/
/* Modbus handling */

typedef enum {
    conv_NONE = 0,
    conv_F,				/* double as data type */
    conv_U1,				/* uint16_t as data type */
    conv_U2,				/* uint32_t as data type */
} conv_t;

typedef struct {
    uint16_t	addr;			/* modbus address to read */
    uint16_t	len;			/* no of words to read */
    uint32_t	gain;			/* gain of data */
    conv_t	conv;			/* data type and conversion */
    const char* fmt;			/* format conversion */
    const char*	topic;			/* topic of data */
} param_t;


/* Hardcoded data for now */
param_t tab[] =
    {
     /* Always have status first */
     { 32089, 1,    1, conv_U1, "%02x",  "sun/status"		}, /*must be 0*/
     { 32087, 1,   10, conv_F,  "%.1lf", "sun/internalTemp"	}, /*must be 1*/
     { 32080, 2, 1000, conv_F,  "%.3lf", "sun/activePower"	},
     { 32064, 2, 1000, conv_F,  "%.3lf", "sun/inputPower"	},
     { 32106, 2,  100, conv_F,  "%.2lf", "sun/accEnergy"	},
     { 32114, 2,  100, conv_F,  "%.2lf", "sun/dailyEnergy"	},

     { 0,0,0,0,0,0}			/* Terminator */
};

/* Max no of parameters to read out/ */
#define MAX_PARAM	(32)
/* Max length of parameter topic strings */
#define MAX_TOPIC_LEN		(80)

char topic_val[MAX_PARAM][MAX_TOPIC_LEN];


/*
 * States	status
 * IDLE		0xa000	No power from panels.
 * ACTIVE	0x020*	Normal operation.
 * STANDBY	0x000*	Door-step between IDEL and STANDBY
 */

typedef enum {
	      ssSTANDBY = 0,
	      ssACTIVE	= 1,
	      ssIDLE	= 2,
	      ssMAX	= 3
} sunState_t;
const char* ssName[] = {"STANDBY", "ACTIVE", "IDLE", "MAX" };

modbus_t* mb = 0;			/* The modbus instance */

void
mb_init()
{
    int i;

    if ( opt_v )
	printf("Connecting to modbus.\n");
    
    if ( mb )
	return;

    if ( opt_m )
	return;
    
    /* Reconnect. */
    mb = modbus_new_tcp( modbus_addr, modbus_port);
    if ( !mb ) {
	fprintf(stderr,"Failed to create MB context.\n");
	exit( EXIT_FAILURE );
    }

    i = modbus_connect(mb);
    if ( i ) {
	fprintf(stderr,"MB Connect failed: %s\n",
		modbus_strerror(errno));
	modbus_free(mb);
	exit( EXIT_FAILURE );
    }

    /* Set read time outs */
    modbus_set_response_timeout( mb, modbus_read_timeout, 0 );
    modbus_set_byte_timeout( mb, modbus_read_timeout, 0 );
    
    modbus_set_slave( mb, modbus_id );

}

#define ALEN (128)
uint16_t  A[ ALEN ];			/* The input buffer */

sunState_t
mbrdr_read()
{
    int i;
    int nr;
    uint32_t status = 0;
    sunState_t ret_val = ssSTANDBY;
    
    /* Clear topic values */
    for ( i = 0; i < MAX_PARAM; ++i )
	topic_val[i][0] = '\0';

    /* Read all values.  Starting with the status. */
    if ( opt_v )
	printf("Reading...\n");

    for ( i = 0; tab[i].addr; ++i ) {

	if ( opt_m ) {			/* Fake reading if in non-modbus mode */
	    nr = 1;
	    A[0] = 0;
	    A[1] = 0;
	    A[2] = 0;
	    A[3] = 0;
	    A[4] = 0;
	    A[5] = 0;
	    A[6] = 0;
	    A[7] = 0;
	}
	else {
	    nr = modbus_read_registers(mb, tab[i].addr, tab[i].len, A );
	}
	
	if ( nr < 1  ) {
	    /* If read fails, bomb after the third time... */
	    static unsigned err_ctr = 0;
	    fprintf( stderr, "modbus_read_failed: %s (nr=%d) ",
		     modbus_strerror(errno), nr);
	    fprintf( stderr, " i=%d %s\n",i,tab[i].topic);
	    ++err_ctr;
	    if ( err_ctr > 3 )
		exit( EXIT_FAILURE );
	}
	else {
	    int p = 0;
	    uint32_t uval = 0;
	    double fval = 0;
	    

	    /* Unpack to a 32bit number */
	    p = 0;
	    uval = A[p]; p++;
	    while ( p < tab[i].len ) {
		uval <<= 16;
		uval |= A[p];
		p++;
	    }

	    /* If i == 0 (status), save status */
	    if ( (i == 0) )
		status = uval;

#if 1
	    /* Enable this if you get read errors... */
	    /* Quick exit if we are in idle state. */
	    if ( (i == 0) && (uval == 0xa000) ) {
		break;
	    }
#endif
	    
	    switch ( tab[i].conv ) {
	    case conv_F:
		fval = uval;
		fval /= tab[i].gain;
		snprintf( topic_val[i], MAX_TOPIC_LEN, tab[i].fmt, fval );
		break;
	    case conv_U1:
	    case conv_U2:
		snprintf( topic_val[i], MAX_TOPIC_LEN, tab[i].fmt, uval );
		break;
	    case conv_NONE:
		break;
	    }
	    topic_val[i][ MAX_TOPIC_LEN-1 ] = '\0';

	    if ( opt_v ) {
		printf("%s=%s [", tab[i].topic, topic_val[i]);
		for ( p = 0; p < tab[i].len; ++p ) {
		    printf("%04x",A[p]);
		}
		printf("]\n");
	    }
	}

	/* Sleep delay before next read */
	sleep(modbus_write_delay);
    }

    /* Invalidate internal temperature if status os 0xa000 */
    /* It's not read out, a value 0 is returned no matter. */
    if ( status == 0xa000 ) {
	topic_val[1][0] = '\0';
    }

    if ( opt_v )
	printf("Done Reading...\n");

    if ( status == 0xa000 ) {
	ret_val = ssIDLE;
    }
    else if ( (status & 0xfff0) == 0x0200 ) {
	ret_val = ssACTIVE;
    }
    else {
	ret_val = ssSTANDBY;
    }
    
    return ret_val;
}



const unsigned max_standby_counter = 20;

void mq_publish();

void
mbrdr_loop()
/* Main loop to read from the modbus */
{
    sunState_t sun_state = ssSTANDBY;	/* start in the STANDBY, for no reason*/
    sunState_t nxt_state = ssSTANDBY;
    
    struct timespec ts_start;
    struct timespec ts_sleep;

    unsigned sleep_interval;
    unsigned standby_counter = 0;
    
    for(;;) {
	my_gettime( &ts_start );
	
	if ( !mb ) {			/* No modbus connection */
	    mb_init();
	}

	/* Always sleep here */
	sleep( modbus_connect_delay  );

	/* Read */
	switch ( sun_state ) {
	case ssSTANDBY:
	    if ( opt_v )
		printf("State=STANDBY\n");
	    nxt_state = mbrdr_read();
	    break;
	case ssACTIVE:
	    if ( opt_v )
		printf("State=ACTIVE\n");
	    nxt_state = mbrdr_read();
	    break;
	case ssIDLE:
	    if ( opt_v )
		printf("State=IDLE\n");
	    nxt_state = mbrdr_read();
	    break;
	default:
	    fprintf(stderr,"BAD state.  Aborting!\n");
	    exit( EXIT_FAILURE );
	}

	mq_publish();		/* Publish all topics */

	switch ( nxt_state ) {
	case ssSTANDBY:
	    ++standby_counter;
	    sleep_interval = STANDBY_interval;
	    /* Force into IDLE */
	    if ( standby_counter > max_standby_counter ) {
		nxt_state = ssIDLE;
		standby_counter = 0;
	    }
	    break;
	case ssACTIVE:
	    standby_counter = 0;
	    sleep_interval = ACTIVE_interval;
	    break;
	case ssIDLE:
	    standby_counter = 0;
	    sleep_interval = IDLE_interval;
	    break;
	default:
	    fprintf(stderr,"BAD next state.  Aborting!\n");
	    exit( EXIT_FAILURE );
	}
	if ( nxt_state != sun_state ) {
	    printf("State change: %s -> %s\n",
		   ssName[sun_state], ssName[nxt_state]);
	}
	sun_state = nxt_state;

	/* Reset connection if in idle... */
	if ( sun_state == ssIDLE ) {
	    modbus_close(mb);
	    modbus_free(mb);
	    mb = 0;
	}
	
	add_time_sec( &ts_sleep, &ts_start, sleep_interval );
	my_sleep( &ts_sleep );
    }
}



/*****************************************************************************/
/* Mosquitto handling */

/* Global mosquitto handle. */
struct mosquitto* mqc = 0;


void
mq_publish()
{
    int i;
    for ( i = 0; tab[i].addr; ++i ) {
	size_t l;
	int status;
	l = strnlen( topic_val[i], MAX_TOPIC_LEN );

	if ( opt_v ) {
	    printf(" %s %s\n", tab[i].topic, topic_val[i] );
	}

	if ( !opt_n && i && (l >0) ) {
	    status = mosquitto_publish(mqc, 0,
				       tab[i].topic,
				       l,
				       topic_val[i], 1, true );
	    if ( status != MOSQ_ERR_SUCCESS) {
		perror("mosquitto_publish: ");
		exit( EXIT_FAILURE );
	    }

	    status = mosquitto_loop_write( mqc, 1 );
	}
    }
}


void
mq_connect_callback(struct mosquitto *mqc, void *obj, int result)
{
    printf("MQ Connected: %d\n", result);
    if ( result != 0 ) {
	/* Something is wrong.  Wait before retry */
	sleep(5);
    }
}


void
mq_disconnect_callback(struct mosquitto *mqc, void *obj, int result)
{
    printf("MQ Disonnected: %d\n", result);
}



void
mq_init()
{
    int i;
    if ( opt_v )
	printf("mq_init()\n");
    i = mosquitto_lib_init();
    if ( i != MOSQ_ERR_SUCCESS) {
	perror("mosquitto_lib_init: ");
	exit( EXIT_FAILURE );
    }
    
    mqc = mosquitto_new(mqtt_id, true, 0);
    if ( !mqc ) {
	perror("mosquitto_new: ");
	exit( EXIT_FAILURE );
    }

    mosquitto_connect_callback_set(mqc, mq_connect_callback);
    mosquitto_disconnect_callback_set(mqc, mq_disconnect_callback);

    i = mosquitto_connect(mqc, mqtt_broker, mqtt_port, 60);
    if ( i != MOSQ_ERR_SUCCESS) {
	perror("mosquitto_connect: ");
	exit( EXIT_FAILURE );
    }


}


void
mq_fini()
{
    int i;
    if ( opt_v )
	printf("mq_fini()\n");

    if ( mqc ) {
	mosquitto_destroy(mqc);
	mqc = 0;
    }

    i = mosquitto_lib_cleanup();
    if ( i != MOSQ_ERR_SUCCESS) {
	perror("mosquitto_lib_cleanup: ");
	exit( EXIT_FAILURE );
    }
}




/*****************************************************************************/

void
print_usage()
{
    fprintf(stderr,"Usage: mbrdr [-v] [-n] config-file\n");
    exit( EXIT_FAILURE );
}


int
main( int argc, const char** argv )
{
    int i;

    setbuf( stdout, 0 );		/* No buffering */

    chdir("/var/local/mbrdr");

    /* Set default values for the MQTT server. */
    mqtt_id = strdup( MQTT_ID );
    mqtt_broker = strdup( MQTT_BROKER );
    mqtt_port = MQTT_PORT;

    modbus_addr = strdup( MODBUS_ADDR );

    
    --argc; ++argv;			/* Jump over first arg */
    while( argc && *argv && **argv ) {
	if ( !strcmp("-v", *argv) ) {
	    opt_v = 1;
	    if ( opt_v )
		printf("Verbose mode.\n");
	}
	else if ( !strcmp("-n", *argv) ) {
	    opt_n = 1;
	    if ( opt_v )
		printf("No-Active mode.\n");
	}
	else if ( !strcmp("-m", *argv) ) {
	    opt_m = 1;
	    if ( opt_v )
		printf("No-Modbus mode.\n");
	}
	else if ( **argv == '-' ) {
	    printf("Illegal argument.");
	    print_usage();
	}
	else if ( argc != 1 ) {
	    printf("Illegal argument.");
	    print_usage();
	    break;
	}
	else {
	    /* Read config file */
	    int status = cfgf_read_file( *argv, tagtab );
	    if ( status ) {
		fprintf(stderr,"Errors in config file.\n");
		exit( EXIT_FAILURE );
	    }
	}
	--argc; ++argv;
    }

    printf("Starting.\n");

    mq_init();

    /* Run the network loop in a background thread, call returns quickly. */
    i = mosquitto_loop_start(mqc);
    if(i != MOSQ_ERR_SUCCESS){
	mosquitto_destroy(mqc);
	fprintf(stderr, "Error: %s\n", mosquitto_strerror(i));
	return 1;
    }

    mbrdr_loop();
    
    mq_fini();

        
    printf("Ending.\n");

    /* Should not come here */
    return EXIT_FAILURE;
}
