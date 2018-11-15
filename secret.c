#include <minix/drivers.h>
#include <minix/driver.h>
#include <stdio.h>
#include <stdlib.h>
#include <minix/ds.h>
#include "secret.h"

/*
 * Function prototypes for the secret driver.
 */
FORWARD _PROTOTYPE( char * secret_name, (void));
FORWARD _PROTOTYPE( int secret_open,      (struct driver *d, message *m) );
FORWARD _PROTOTYPE( int secret_close,     (struct driver *d, message *m) );
FORWARD _PROTOTYPE( int secret_ioctl, (struct driver *d, message *m));
FORWARD _PROTOTYPE( struct device * secret_prepare, (int device) );
FORWARD _PROTOTYPE( int secret_transfer,  (int procnr, int opcode,
                                          u64_t position, iovec_t *iov,
                                          unsigned nr_req) );
FORWARD _PROTOTYPE( void secret_geometry, (struct partition *entry) );

/* SEF functions and variables. */
FORWARD _PROTOTYPE( void sef_local_startup, (void) );
FORWARD _PROTOTYPE( int sef_cb_init, (int type, sef_init_info_t *info) );
FORWARD _PROTOTYPE( int sef_cb_lu_state_save, (int) );
FORWARD _PROTOTYPE( int lu_state_restore, (void) );

#define SECRET_SIZE 8192
#define TRUE 1
#define FALSE 0
#define O_WRONLY 2
#define O_RDONLY 4
#define O_RDWR 6
#define UNOWNED -1 

PRIVATE uid_t secretHolder = UNOWNED;
PRIVATE vir_bytes currSecretSize = 0;
PRIVATE char secret[SECRET_SIZE];
PRIVATE int currWritePlace = 0, currReadPlace = 0; 

/* Entry points to the hello driver. */
PRIVATE struct driver secret_tab =
{
    secret_name,
    secret_open,
    secret_close,
    secret_ioctl,
    nop_ioctl,
    secret_prepare,
    secret_transfer,
    nop_cleanup,
    secret_geometry,
    nop_alarm,
    nop_cancel,
    nop_select,
    nop_ioctl,
    do_nop,
};

/** Represents the /dev/hello device. */
PRIVATE struct device secret_device;

/** State variable to count the number of times the device has been opened. */
PRIVATE int open_counter;
PRIVATE int open_fds;

PRIVATE char * secret_name(void)
{
    return "secret";
}

PRIVATE int secret_open(
    struct driver *d, message *m) {
	struct ucred user;
	if(m->COUNT == O_RDWR) {
		return EACCES;
	}

	getnucred(m->IO_ENDPT, &user);
	if(secretHolder == UNOWNED) {
		if(m->COUNT == O_WRONLY) {
			secretHolder = user.uid;
			return OK;
		}else if(m->COUNT == O_RDONLY) {
			secretHolder = user.uid;	
			open_fds++;
			return OK;
		} else {
			return -1;
		}
	} else {
		if(m->COUNT == O_RDONLY) {
			if(secretHolder == user.uid) {
				return OK;	
			} else {
				return EACCES;
			}
		} else if(m->COUNT == O_WRONLY){
			return ENOSPC;	
		}
	}
	return OK;
}

PRIVATE int secret_close(struct driver *d, message *m) {

   /* No more processes using secret, so clear it out */
   int i;
   open_fds -= 1;
   if (open_fds == 0) {
      for (i = 0; i < SECRET_SIZE; i++) {
         secret[i] = '\0';
      }
      secretHolder = UNOWNED;
   }
   return OK;
}

PRIVATE int secret_ioctl(struct driver *d, message *m) {
   int res;
   uid_t grantee;

   if (m->REQUEST != SSGRANT) {
      return ENOTTY;
   }

   res = sys_safecopyfrom(m->IO_ENDPT, (vir_bytes)m->IO_GRANT,
                          0, (vir_bytes)&grantee, sizeof(grantee), D);

   secretHolder = grantee;

   return OK;
}

PRIVATE struct device * secret_prepare(dev)
    int dev;
{
    secret_device.dv_base.lo = 0;
    secret_device.dv_base.hi = 0;
    secret_device.dv_size.lo = strlen(secret);
    secret_device.dv_size.hi = 0;
    return &secret_device;
}

PRIVATE int secret_transfer(proc_nr, opcode, position, iov, nr_req)
    int proc_nr;
    int opcode;
    u64_t position;
    iovec_t *iov;
    unsigned nr_req;
{
    int readBytes, writeBytes, ret;
    switch (opcode)
    {
        case DEV_GATHER_S:
            readBytes = iov->iov_size;
            if(readBytes <= 0) {
                return OK;
            }
            if(readBytes > (currWritePlace - currReadPlace) {
                readBytes = currWritePlace - currReadPlace;
            }
            ret = sys_safecopyto(proc_nr, iov->iov_addr, 0,
                                (vir_bytes) (secret + currReadPlace),
                                 readBytes, D);
            iov->iov_size -= readBytes;
            currReadPlace += readBytes;
            break;

	    case DEV_SCATTER_S:
            writeBytes = iov->iov_size;
            if(writeBytes <= 0) {
                return OK;
            }
            if(writeBytes > SECRET_SIZE-currWritePlace) {
                writeBytes = SECRET_SIZE-currWritePlace;
            }
	        ret = sys_safecopyfrom(proc_nr, iov->iov_addr, 0, 
                                  (vir_bytes) (secret + currWritePlace),
				                    writeBytes, D);
	        iov->iov_size += writeBytes;
            currWritePlace += writeBytes
        default:
            return EINVAL;
    }
    return ret;
}

PRIVATE void secret_geometry(entry)
    struct partition *entry;
{
    entry->cylinders = 0;
    entry->heads     = 0;
    entry->sectors   = 0;
}

PRIVATE int sef_cb_lu_state_save(int state) {
/* Save the state. */
    ds_publish_u32("open_counter", open_counter, DSF_OVERWRITE);

    return OK;
}

PRIVATE int lu_state_restore() {
/* Restore the state. */
    u32_t value;

    ds_retrieve_u32("open_counter", &value);
    ds_delete_u32("open_counter");
    open_counter = (int) value;

    return OK;
}

PRIVATE void sef_local_startup()
{
    /*
     * Register init callbacks. Use the same function for all event types
     */
    sef_setcb_init_fresh(sef_cb_init);
    sef_setcb_init_lu(sef_cb_init);
    sef_setcb_init_restart(sef_cb_init);

    /*
     * Register live update callbacks.
     */
    /* - Agree to update immediately when LU is requested in a valid state. */
    sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
    /* - Support live update starting from any standard state. */
    sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);
    /* - Register a custom routine to save the state. */
    sef_setcb_lu_state_save(sef_cb_lu_state_save);

    /* Let SEF perform startup. */
    sef_startup();
}

PRIVATE int sef_cb_init(int type, sef_init_info_t *info)
{
/* Initialize the hello driver. */
    int do_announce_driver = TRUE;

    open_counter = 0;
    switch(type) {
        case SEF_INIT_FRESH:
            printf("%s", secret);
        break;

        case SEF_INIT_LU:
            /* Restore the state. */
            lu_state_restore();
            do_announce_driver = FALSE;

            printf("%sHey, I'm a new version!\n", secret);
        break;

        case SEF_INIT_RESTART:
            printf("%sHey, I've just been restarted!\n", secret);
        break;
    }

    /* Announce we are up when necessary. */
    if (do_announce_driver) {
        driver_announce();
    }

    /* Initialization completed successfully. */
    return OK;
}

PUBLIC int main(int argc, char **argv)
{
    /*
     * Perform initialization.
     */
    sef_local_startup();

    /*
     * Run the main loop.
     */
    driver_task(&secret_tab, DRIVER_STD);
    return OK;
}

